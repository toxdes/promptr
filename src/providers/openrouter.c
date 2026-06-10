#include "config.h"
#include "configfile.h"
#include "http.h"
#include "provider.h"
#include "tab.h"
#include "window.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define OPENROUTER_DEFAULT_MODEL "openai/gpt-4o-mini"

/* ── types ────────────────────────────────────────────────────── */

#define HEARTBEAT_TIMEOUT_US     (90 * G_TIME_SPAN_SECOND)

typedef struct {
  char *api_key;
  char *base_url;

  /* Per-request state (single active request at a time) */
  Tab *active_tab;
  GString *sse_buffer;
  char *full_output;
  gboolean done_received;
  gboolean saw_data_event; /* true once we get a non-empty content delta */
  gint64 start_time;
  gint64 last_event_time; /* monotonic time of last SSE event (any type) */
  ProviderCallback callback;
  gpointer callback_data;
} OpenRouterImpl;

/* ── forward declarations ─────────────────────────────────────── */

static gboolean openrouter_init(void **impl, RuntimeConfig *config,
                                GError **error);
static void openrouter_destroy(void *impl);
static void openrouter_cleanup_session(void *impl, gpointer tab_key);
static gboolean
openrouter_submit(void *impl, gpointer tab_key, const char *query,
                  const ProviderMessage *history, int n_history,
                  const char *model, const char *agent, gboolean is_follow_up,
                  GCancellable *cancellable, ProviderCallback callback,
                  gpointer callback_data, GError **error);
static void openrouter_cancel(void *impl);
static void openrouter_cancel_tab(void *impl, gpointer tab_key);
static char *openrouter_get_display_string(void *impl, const char *query,
                                           const char *model, const char *agent,
                                           gboolean is_follow_up);
static char **openrouter_get_models(void *impl);
static char **openrouter_get_agents(void *impl);

/* SSE / JSON helpers */
static void on_or_chunk(const char *data, gsize len, gpointer user);
static void on_or_done(GError *error, long response_code,
                       const char *content_type, gpointer user);
static void process_sse(OpenRouterImpl *ori);
static char *sse_extract_content(const char *json_line, Tab *debug_tab);
static char *sse_extract_reasoning(const char *json_line);
static gboolean openrouter_free_idle(gpointer user_data);

/* ── vtable ───────────────────────────────────────────────────── */

ProviderVTable openrouter_vtable = {
    .name = "openrouter",
    .init = openrouter_init,
    .destroy = openrouter_destroy,
    .cleanup_session = openrouter_cleanup_session,
    .submit = openrouter_submit,
    .cancel = openrouter_cancel,
    .cancel_tab = openrouter_cancel_tab,
    .get_display_string = openrouter_get_display_string,
    .get_models = openrouter_get_models,
    .get_agents = openrouter_get_agents,
    .needs_full_history = TRUE,
};

/* ── init / destroy ───────────────────────────────────────────── */

static gboolean openrouter_init(void **impl, RuntimeConfig *config,
                                GError **error) {
  OpenRouterImpl *ori;

  ori = g_new0(OpenRouterImpl, 1);

  /* API key: env var takes precedence over config */
  {
    const char *env = getenv("OPENROUTER_API_KEY");

    if (env != NULL && env[0] != '\0')
      ori->api_key = g_strdup(env);
    else
      ori->api_key =
          runtime_config_get_string(config, "openrouter_api_key", "");

    if (ori->api_key == NULL || ori->api_key[0] == '\0') {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "OPENROUTER_API_KEY not set — configure it in "
                  "~/.config/" DATA_DIR_SUFFIX "/config or set the env var");
      goto fail;
    }
  }

  ori->base_url = g_strdup("https://openrouter.ai/api/v1");
  *impl = ori;
  return TRUE;

fail:
  g_free(ori->api_key);
  g_free(ori);
  return FALSE;
}

static void openrouter_cancel_request(OpenRouterImpl *ori);

static void openrouter_destroy(void *impl) {
  OpenRouterImpl *ori = impl;

  if (ori == NULL)
    return;

  /* Cancel any in-flight HTTP request and make on_or_done a no-op */
  openrouter_cancel_request(ori);
  ori->callback = NULL;
  ori->active_tab = NULL;

  /* Schedule actual free via idle — ensures any already-queued
     dispatch_done callback runs before our memory is gone, since
     idle callbacks are processed FIFO on the same main loop. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, openrouter_free_idle, ori, NULL);
}

static gboolean openrouter_free_idle(gpointer user_data) {
  OpenRouterImpl *ori = user_data;

  if (ori->sse_buffer != NULL)
    g_string_free(ori->sse_buffer, TRUE);
  g_free(ori->full_output);
  g_free(ori->api_key);
  g_free(ori->base_url);
  g_free(ori);
  return G_SOURCE_REMOVE;
}

static void openrouter_cleanup_session(void *impl, gpointer tab_key) {
  OpenRouterImpl *ori = impl;

  if (ori->active_tab == tab_key) {
    openrouter_cancel_request(ori);
    ori->active_tab = NULL;
    ori->callback = NULL;
  }
}

/* Cancel the active HTTP request by cancelling the tab's GCancellable */
static void openrouter_cancel_request(OpenRouterImpl *ori) {
  if (ori->active_tab != NULL && ori->active_tab->cancellable != NULL)
    g_cancellable_cancel(ori->active_tab->cancellable);
}

/* ── submit ───────────────────────────────────────────────────── */

static gboolean
openrouter_submit(void *impl, gpointer tab_key, const char *query,
                  const ProviderMessage *history, int n_history,
                  const char *model, const char *agent, gboolean is_follow_up,
                  GCancellable *cancellable, ProviderCallback callback,
                  gpointer callback_data, GError **error) {
  OpenRouterImpl *ori = impl;
  Tab *tab = tab_key;
  const char *model_str;
  char url[256];
  GString *body;

  (void)is_follow_up;
  (void)query;

  if (ori->api_key == NULL || ori->api_key[0] == '\0') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "OpenRouter API key not configured");
    return FALSE;
  }

  /* Reset per-request state */
  if (ori->sse_buffer != NULL)
    g_string_free(ori->sse_buffer, TRUE);
  ori->sse_buffer = g_string_new(NULL);
  g_free(ori->full_output);
  ori->full_output = NULL;
  ori->done_received = FALSE;
  ori->saw_data_event = FALSE;
  ori->start_time = g_get_monotonic_time();
  ori->last_event_time = ori->start_time;
  ori->active_tab = tab;
  ori->callback = callback;
  ori->callback_data = callback_data;

  model_str =
      (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
          ? model
          : OPENROUTER_DEFAULT_MODEL;

  /* Build JSON body via json-glib — handles escaping, encoding, structure. */
  {
    JsonBuilder *jb = json_builder_new();
    JsonGenerator *gen;
    JsonNode *root;
    gsize data_len;

    json_builder_begin_object(jb);

    json_builder_set_member_name(jb, "model");
    json_builder_add_string_value(jb, model_str);

    json_builder_set_member_name(jb, "stream");
    json_builder_add_boolean_value(jb, TRUE);

    json_builder_set_member_name(jb, "messages");
    json_builder_begin_array(jb);

    if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0') {
      json_builder_begin_object(jb);
      json_builder_set_member_name(jb, "role");
      json_builder_add_string_value(jb, "system");
      json_builder_set_member_name(jb, "content");
      json_builder_add_string_value(jb, agent);
      json_builder_end_object(jb);
    }

    for (int i = 0; i < n_history; i++) {
      const char *role_str;

      switch (history[i].role) {
      case PROVIDER_ROLE_ASSISTANT:
        role_str = "assistant";
        break;
      case PROVIDER_ROLE_SYSTEM:
        role_str = "system";
        break;
      default:
        role_str = "user";
        break;
      }

      if (history[i].content != NULL && history[i].content[0] != '\0') {
        json_builder_begin_object(jb);
        json_builder_set_member_name(jb, "role");
        json_builder_add_string_value(jb, role_str);
        json_builder_set_member_name(jb, "content");
        json_builder_add_string_value(jb, history[i].content);
        json_builder_end_object(jb);
      }
    }

    json_builder_end_array(jb);
    json_builder_end_object(jb);

    root = json_builder_get_root(jb);
    gen = json_generator_new();
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    {
      g_autofree char *json_data = json_generator_to_data(gen, &data_len);

      body = g_string_new(json_data);
    }
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(jb);
  }

  /* Build headers */
  g_autofree char *auth_header =
      g_strdup_printf("Authorization: Bearer %s", ori->api_key);
  const char *headers[] = {"Content-Type: application/json", auth_header, NULL};

  /* Build URL */
  g_snprintf(url, sizeof(url), "%s/chat/completions", ori->base_url);

  /* Log HTTP request */
  if (tab != NULL && tab->win != NULL) {
    int extra_system =
        (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
            ? 1
            : 0;

    log_append(tab->win, "openrouter → POST %s model=%s msgs=%d body=%s", url,
               model_str, n_history + extra_system, body->str);
  }

  /* Make streaming HTTP call */
  http_post_stream(url, headers, body->str, body->len, cancellable, on_or_chunk,
                   on_or_done, ori);

  g_string_free(body, TRUE);

  return TRUE;
}

/* ── cancel ───────────────────────────────────────────────────── */

static void openrouter_cancel(void *impl) {
  OpenRouterImpl *ori = impl;

  if (ori->active_tab != NULL && ori->active_tab->cancellable != NULL) {
    g_cancellable_cancel(ori->active_tab->cancellable);
    ori->active_tab = NULL;
  }
}

static void openrouter_cancel_tab(void *impl, gpointer tab_key) {
  OpenRouterImpl *ori = impl;

  if (ori->active_tab == tab_key && ori->active_tab != NULL &&
      ori->active_tab->cancellable != NULL) {
    g_cancellable_cancel(ori->active_tab->cancellable);
    ori->active_tab = NULL;
  }
}

/* ── SSE / HTTP callbacks ─────────────────────────────────────── */

static void on_or_chunk(const char *data, gsize len, gpointer user) {
  OpenRouterImpl *ori = user;

  if (ori->done_received)
    return;

  g_string_append_len(ori->sse_buffer, data, len);

  /* Heartbeat timeout: if no data event within HEARTBEAT_TIMEOUT,
     assume the upstream model never started and abort. */
  ori->last_event_time = g_get_monotonic_time();
  if (!ori->saw_data_event &&
      ori->last_event_time - ori->start_time > HEARTBEAT_TIMEOUT_US) {
    /* Cancel the HTTP request first */
    if (ori->active_tab != NULL && ori->active_tab->cancellable != NULL)
      g_cancellable_cancel(ori->active_tab->cancellable);

    if (ori->active_tab != NULL && ori->active_tab->win != NULL)
      log_append(ori->active_tab->win,
                 "openrouter ← heartbeat timeout (no data for %ds)",
                 (int)(HEARTBEAT_TIMEOUT_US / G_TIME_SPAN_SECOND));
    if (ori->callback != NULL && ori->active_tab != NULL) {
      ProviderEvent ev = {
          PROVIDER_EVENT_ERROR, NULL,
          (char *)"OpenRouter sent only heartbeats for "
                  "30s — model is queued. Try openai/gpt-4o-mini.",
          ori->last_event_time - ori->start_time};
      ori->callback(ori->active_tab, &ev, ori->callback_data);
    }
    /* Null these so the delayed on_or_done callback becomes a no-op */
    ori->active_tab = NULL;
    ori->callback = NULL;
    return;
  }

  process_sse(ori);
}

static void on_or_done(GError *error, long response_code,
                       const char *content_type, gpointer user) {
  OpenRouterImpl *ori = user;
  Tab *tab = ori->active_tab;
  gint64 elapsed;

  /* If callback was nulled by heartbeat timeout, this is a delayed
     curl completion — ignore it. */
  if (ori->callback == NULL)
    return;

  elapsed = g_get_monotonic_time() - ori->start_time;

  if (tab != NULL && tab->win != NULL)
    log_append(
        tab->win,
        "openrouter ← HTTP %ld Content-Type: %s elapsed=%" G_GINT64_FORMAT,
        response_code, content_type != NULL ? content_type : "N/A", elapsed);

  /* Log HTTP error body for non-2xx responses (OpenRouter error message) */
  if (response_code >= 300 && tab != NULL && tab->win != NULL &&
      ori->sse_buffer != NULL && ori->sse_buffer->len > 0) {
    /* Extract first 2KB of error body for diagnosis */
    gsize log_len = ori->sse_buffer->len;
    char *body =
        g_strndup(ori->sse_buffer->str, log_len > 2048 ? 2048 : log_len);
    log_append(tab->win, "openrouter ← error body: %s%s", body,
               log_len > 2048 ? "..." : "");
    g_free(body);
  }

  if (error != NULL) {
    if (tab != NULL && tab->win != NULL)
      log_append(tab->win, "openrouter ← error: %s",
                 error->message != NULL ? error->message : "unknown");

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      ProviderEvent ev = {PROVIDER_EVENT_ERROR, NULL, (char *)"", elapsed};

      if (ori->callback != NULL)
        ori->callback(tab, &ev, ori->callback_data);
    } else {
      ProviderEvent ev = {
          PROVIDER_EVENT_ERROR, NULL,
          (char *)(error->message != NULL ? error->message : "HTTP error"),
          elapsed};

      if (ori->callback != NULL)
        ori->callback(tab, &ev, ori->callback_data);
    }

    g_string_free(ori->sse_buffer, TRUE);
    ori->sse_buffer = NULL;
    g_free(ori->full_output);
    ori->full_output = NULL;
    return;
  }

  /* Process any remaining SSE data (content was already delivered
     piece-by-piece via CHUNK events during streaming). */
  process_sse(ori);

  if (tab != NULL && tab->win != NULL)
    log_append(tab->win,
               "openrouter ← done elapsed=%" G_GINT64_FORMAT " output_len=%zu",
               elapsed,
               ori->full_output != NULL ? strlen(ori->full_output) : 0);

  /* Signal completion */
  if (ori->callback != NULL) {
    ProviderEvent done = {PROVIDER_EVENT_DONE,
                          ori->full_output != NULL ? ori->full_output : "",
                          NULL, elapsed};

    ori->callback(tab, &done, ori->callback_data);
  }

  ori->active_tab = NULL;
  g_string_free(ori->sse_buffer, TRUE);
  ori->sse_buffer = NULL;
  g_free(ori->full_output);
  ori->full_output = NULL;
}
/* ── SSE parser ───────────────────────────────────────────────── */

static void process_sse(OpenRouterImpl *ori) {
  char *p;
  char *event_end;

  if (ori->sse_buffer == NULL)
    return;

  p = ori->sse_buffer->str;

  for (;;) {
    int skip;
    char *segment;
    const char *data_val;

    /* Find SSE event separator: blank line */
    event_end = strstr(p, "\r\n\r\n");
    if (event_end != NULL) {
      skip = 4;
    } else {
      event_end = strstr(p, "\n\n");
      if (event_end == NULL)
        break;
      skip = 2;
    }

    /* Copy the event line */
    segment = g_strndup(p, event_end - p);

    /* Find "data:" prefix (OpenRouter uses "data: {...}") */
    data_val = strstr(segment, "data:");
    if (data_val != NULL) {
      /* Skip past "data:" and any whitespace */
      data_val += 5;
      while (*data_val == ' ' || *data_val == '\t')
        data_val++;

      if (strcmp(data_val, "[DONE]") == 0) {
        ori->done_received = TRUE;
      } else {
        char *content = sse_extract_content(data_val, ori->active_tab);

        if (content != NULL && content[0] != '\0') {
          /* Accumulate full output */
          if (ori->full_output == NULL)
            ori->full_output = g_strdup(content);
          else {
            char *old = ori->full_output;
            ori->full_output = g_strdup_printf("%s%s", old, content);
            g_free(old);
          }

          ori->saw_data_event = TRUE;

          if (ori->active_tab != NULL && ori->active_tab->win != NULL)
            log_append(ori->active_tab->win, "openrouter ← text: %.*s",
                       (int)(strlen(content) < 120 ? strlen(content) : 120),
                       content);

          /* Emit CHUNK synchronously — GTK will redraw on next frame */
          if (ori->callback != NULL && ori->active_tab != NULL) {
            ProviderEvent chunk = {PROVIDER_EVENT_CHUNK, content, NULL, 0};

            ori->callback(ori->active_tab, &chunk, ori->callback_data);
          }
        } else {
          /* No content — check for reasoning tokens, log only */
          char *reasoning = sse_extract_reasoning(data_val);

          if (reasoning != NULL) {
            if (ori->active_tab != NULL && ori->active_tab->win != NULL)
              log_append(
                  ori->active_tab->win, "openrouter ← reasoning: %.*s",
                  (int)(strlen(reasoning) < 200 ? strlen(reasoning) : 200),
                  reasoning);
            g_free(reasoning);
          }
        }
        g_free(content);
      }
    }

    g_free(segment);
    p = event_end + skip;
  }

  /* Keep remaining partial data for next invocation */
  {
    GString *remaining = g_string_new(p);
    g_string_free(ori->sse_buffer, TRUE);
    ori->sse_buffer = remaining;
  }
}

/* ── SSE delta JSON parser (via json-glib) ───────────────────── */

static char *sse_extract_content(const char *json_line, Tab *debug_tab) {
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonObject *obj;
  JsonArray *choices;
  JsonNode *choice_node;
  JsonObject *choice_obj;
  JsonObject *delta;
  const char *content;

  (void)debug_tab;

  if (json_line == NULL || json_line[0] == '\0')
    return NULL;

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_line, -1, &error))
    return NULL;

  root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
    return NULL;

  obj = json_node_get_object(root);
  choices = json_object_get_array_member(obj, "choices");
  if (choices == NULL || json_array_get_length(choices) == 0)
    return NULL;

  choice_node = json_array_get_element(choices, 0);
  if (choice_node == NULL || JSON_NODE_HOLDS_VALUE(choice_node))
    return NULL;

  choice_obj = json_node_get_object(choice_node);
  delta = json_object_get_object_member(choice_obj, "delta");
  if (delta == NULL)
    return NULL;

  if (json_object_has_member(delta, "content")) {
    content = json_object_get_string_member(delta, "content");

    if (content != NULL && content[0] != '\0')
      return g_strdup(content);
  }

  return NULL;
}

/* ── SSE reasoning extractor (log-only, never displayed) ──────── */

static char *sse_extract_reasoning(const char *json_line) {
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonObject *obj, *delta;
  JsonArray *choices;
  JsonNode *choice_node;
  JsonObject *choice_obj;

  if (json_line == NULL || json_line[0] == '\0')
    return NULL;

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_line, -1, &error))
    return NULL;

  root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
    return NULL;

  obj = json_node_get_object(root);
  choices = json_object_get_array_member(obj, "choices");
  if (choices == NULL || json_array_get_length(choices) == 0)
    return NULL;

  choice_node = json_array_get_element(choices, 0);
  if (choice_node == NULL || JSON_NODE_HOLDS_VALUE(choice_node))
    return NULL;

  choice_obj = json_node_get_object(choice_node);
  delta = json_object_get_object_member(choice_obj, "delta");
  if (delta == NULL)
    return NULL;

  if (json_object_has_member(delta, "reasoning")) {
    const char *reasoning = json_object_get_string_member(delta, "reasoning");

    if (reasoning != NULL && reasoning[0] != '\0')
      return g_strdup(reasoning);
  }

  return NULL;
}

/* ── display string ───────────────────────────────────────────── */

static char *openrouter_get_display_string(void *impl, const char *query,
                                           const char *model, const char *agent,
                                           gboolean is_follow_up) {
  const char *model_str;
  GString *s;

  (void)impl;
  (void)agent;
  (void)is_follow_up;

  model_str =
      (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
          ? model
          : OPENROUTER_DEFAULT_MODEL;

  s = g_string_new("OpenRouter [model: ");
  g_string_append(s, model_str);
  g_string_append_printf(s, "] %s", query != NULL ? query : "");
  return g_string_free(s, FALSE);
}

/* ── model / agent lists ─────────────────────────────────────── */

static char **openrouter_get_models(void *impl) {
  (void)impl;
  return NULL; /* loaded from config */
}

static char **openrouter_get_agents(void *impl) {
  (void)impl;
  return NULL;
}
