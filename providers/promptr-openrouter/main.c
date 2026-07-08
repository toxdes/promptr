#include "../../lib/promptr-protocol/jsonrpc.h"
#include <curl/curl.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPENROUTER_DEFAULT_MODEL "openai/gpt-4o-mini"
#define HEARTBEAT_TIMEOUT_US     (90 * G_TIME_SPAN_SECOND)

/* ── state ─────────────────────────────────────────────────────── */

typedef struct {
  char *api_key;
  char *base_url;

  char *active_tab_id;
  char *active_model;

  GString *sse_buffer;
  char *full_output;
  gboolean done_received;
  gboolean saw_data_event;
  gint64 start_time;
  gint64 last_event_time;

  CURLM *multi;
  CURL *easy;
  guint curl_idle_id;
  gboolean cancelled;
} ProviderState;

/* ── forward declarations ──────────────────────────────────────── */

static void write_stream_event(const char *tab_id, const char *type,
                               const char *output);
static void write_error_event(const char *tab_id, const char *msg);
static gboolean on_curl_idle(gpointer user_data);
static void process_sse(ProviderState *state, const char *tab_id);

/* ── helpers ───────────────────────────────────────────────────── */

static void write_stream_event(const char *tab_id, const char *type,
                               const char *output) {
  JsonBuilder *builder = json_builder_new();
  JsonNode *p, *event;

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "tab_id");
  json_builder_add_string_value(builder, tab_id != NULL ? tab_id : "");
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, type);
  json_builder_set_member_name(builder, "output");
  json_builder_add_string_value(builder, output != NULL ? output : "");
  json_builder_end_object(builder);

  p = json_builder_get_root(builder);
  event = json_rpc_make_notification("provider/event", p);
  g_object_unref(builder);

  json_rpc_write(stdout, event, NULL);
  json_node_free(event);
  fflush(stdout);
}

static void write_error_event(const char *tab_id, const char *msg) {
  JsonBuilder *builder = json_builder_new();
  JsonNode *p, *event;

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "tab_id");
  json_builder_add_string_value(builder, tab_id != NULL ? tab_id : "");
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "error");
  json_builder_set_member_name(builder, "error_msg");
  json_builder_add_string_value(builder, msg != NULL ? msg : "");
  json_builder_end_object(builder);

  p = json_builder_get_root(builder);
  event = json_rpc_make_notification("provider/event", p);
  g_object_unref(builder);

  json_rpc_write(stdout, event, NULL);
  json_node_free(event);
  fflush(stdout);
}

/* ── SSE parser (extracted from openrouter.c) ──────────────────── */

static char *sse_extract_content(const char *json_line) {
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonObject *obj;
  JsonArray *choices;
  JsonNode *choice_node;
  JsonObject *choice_obj;
  JsonObject *delta;
  const char *content;

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

static void process_sse(ProviderState *state, const char *tab_id) {
  char *p;
  char *event_end;

  if (state->sse_buffer == NULL)
    return;

  p = state->sse_buffer->str;

  for (;;) {
    int skip;
    char *segment;
    const char *data_val;

    event_end = strstr(p, "\r\n\r\n");
    if (event_end != NULL) {
      skip = 4;
    } else {
      event_end = strstr(p, "\n\n");
      if (event_end == NULL)
        break;
      skip = 2;
    }

    segment = g_strndup(p, event_end - p);

    data_val = strstr(segment, "data:");
    if (data_val != NULL) {
      data_val += 5;
      while (*data_val == ' ' || *data_val == '\t')
        data_val++;

      if (strcmp(data_val, "[DONE]") == 0) {
        state->done_received = TRUE;
      } else {
        char *content = sse_extract_content(data_val);

        if (content != NULL && content[0] != '\0') {
          if (state->full_output == NULL)
            state->full_output = g_strdup(content);
          else {
            char *old = state->full_output;
            state->full_output = g_strdup_printf("%s%s", old, content);
            g_free(old);
          }

          state->saw_data_event = TRUE;
          write_stream_event(tab_id, "chunk", content);
        }
        g_free(content);
      }
    }

    g_free(segment);
    p = event_end + skip;
  }

  /* Keep remaining partial data */
  {
    GString *remaining = g_string_new(p);
    g_string_free(state->sse_buffer, TRUE);
    state->sse_buffer = remaining;
  }
}

/* ── HTTP callbacks ─────────────────────────────────────────────── */

static size_t on_http_data(char *ptr, size_t size, size_t nmemb,
                           void *user_data) {
  ProviderState *state = user_data;
  size_t total = size * nmemb;

  if (state->done_received || state->cancelled)
    return total;

  g_string_append_len(state->sse_buffer, ptr, total);

  state->last_event_time = g_get_monotonic_time();

  if (!state->saw_data_event &&
      state->last_event_time - state->start_time > HEARTBEAT_TIMEOUT_US) {
    write_error_event(state->active_tab_id,
                      "OpenRouter sent only heartbeats for "
                      "30s — model is queued.");
    state->cancelled = TRUE;
    return 0;
  }

  process_sse(state, state->active_tab_id);

  return total;
}

/* ── curl idle handler ──────────────────────────────────────────── */

static gboolean on_curl_idle(gpointer user_data) {
  ProviderState *state = user_data;
  int running_handles;
  CURLMsg *msg;
  int msgs_left;

  if (state->cancelled)
    goto cleanup;

  curl_multi_perform(state->multi, &running_handles);

  if (running_handles > 0)
    return G_SOURCE_CONTINUE;

  /* Transfer complete */
  while ((msg = curl_multi_info_read(state->multi, &msgs_left)) != NULL) {
    if (msg->msg == CURLMSG_DONE) {
      long response_code;

      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                        &response_code);

      if (response_code != 200) {
        g_warning("openrouter-provider: HTTP %ld", response_code);
        write_error_event(state->active_tab_id, "HTTP error");
        goto cleanup;
      }
    }
  }

  process_sse(state, state->active_tab_id);

  write_stream_event(state->active_tab_id, "done",
                     state->full_output != NULL ? state->full_output : "");

cleanup:
  state->curl_idle_id = 0;
  if (state->multi != NULL) {
    curl_multi_cleanup(state->multi);
    state->multi = NULL;
  }
  if (state->easy != NULL) {
    curl_easy_cleanup(state->easy);
    state->easy = NULL;
  }
  g_free(state->active_tab_id);
  state->active_tab_id = NULL;
  g_free(state->active_model);
  state->active_model = NULL;

  return G_SOURCE_REMOVE;
}

/* ── request handlers ───────────────────────────────────────────── */

static gboolean handle_initialize(ProviderState *state, JsonNode *params) {
  JsonObject *obj;
  JsonNode *config_node;
  const char *key = NULL, *url = NULL;

  if (params != NULL && !JSON_NODE_HOLDS_VALUE(params)) {
    obj = json_node_get_object(params);
    if (json_object_has_member(obj, "config")) {
      config_node = json_object_get_member(obj, "config");
      if (config_node != NULL && !JSON_NODE_HOLDS_VALUE(config_node)) {
        JsonObject *cfg = json_node_get_object(config_node);

        if (json_object_has_member(cfg, "api_key"))
          key = json_object_get_string_member(cfg, "api_key");
        if (json_object_has_member(cfg, "base_url"))
          url = json_object_get_string_member(cfg, "base_url");
      }
    }
  }

  if (key == NULL || key[0] == '\0')
    key = getenv("OPENROUTER_API_KEY");

  if (key == NULL || key[0] == '\0') {
    g_warning("openrouter-provider: API key not configured");
    return FALSE;
  }

  state->api_key = g_strdup(key);
  state->base_url =
      g_strdup(url != NULL ? url : "https://openrouter.ai/api/v1");

  fprintf(stderr, "openrouter-provider: using base=%s\n", state->base_url);
  return TRUE;
}

static gboolean handle_submit(ProviderState *state, JsonNode *params) {
  JsonObject *obj;
  const char *tab_id, *model_str;
  JsonBuilder *body;
  g_autofree char *body_json = NULL;
  g_autofree char *auth_header = NULL;
  g_autofree char *url = NULL;
  struct curl_slist *headers = NULL;
  g_autoptr(JsonNode) body_node = NULL;
  size_t body_len;

  if (params == NULL || JSON_NODE_HOLDS_VALUE(params))
    return FALSE;

  obj = json_node_get_object(params);

  if (!json_object_has_member(obj, "tab_id"))
    return FALSE;

  tab_id = json_object_get_string_member(obj, "tab_id");

  model_str = json_object_has_member(obj, "model") &&
                      g_strcmp0(json_object_get_string_member(obj, "model"),
                                "None") != 0
                  ? json_object_get_string_member(obj, "model")
                  : OPENROUTER_DEFAULT_MODEL;

  state->active_tab_id = g_strdup(tab_id);
  state->active_model = g_strdup(model_str);

  /* Reset per-request state */
  if (state->sse_buffer != NULL)
    g_string_free(state->sse_buffer, TRUE);
  state->sse_buffer = g_string_new(NULL);
  g_free(state->full_output);
  state->full_output = NULL;
  state->done_received = FALSE;
  state->saw_data_event = FALSE;
  state->start_time = g_get_monotonic_time();
  state->last_event_time = state->start_time;
  state->cancelled = FALSE;

  /* Build request body using messages from submit params */
  body = json_builder_new();
  json_builder_begin_object(body);
  json_builder_set_member_name(body, "model");
  json_builder_add_string_value(body, model_str);
  json_builder_set_member_name(body, "stream");
  json_builder_add_boolean_value(body, TRUE);

  json_builder_set_member_name(body, "messages");
  json_builder_begin_array(body);

  /* Add system message from agent if present */
  if (json_object_has_member(obj, "agent")) {
    JsonNode *agent_node = json_object_get_member(obj, "agent");

    if (agent_node != NULL && !JSON_NODE_HOLDS_VALUE(agent_node)) {
      JsonObject *agent_obj = json_node_get_object(agent_node);
      const char *agent_content = NULL;

      if (json_object_has_member(agent_obj, "content"))
        agent_content = json_object_get_string_member(agent_obj, "content");

      if (agent_content != NULL && agent_content[0] != '\0') {
        json_builder_begin_object(body);
        json_builder_set_member_name(body, "role");
        json_builder_add_string_value(body, "system");
        json_builder_set_member_name(body, "content");
        json_builder_add_string_value(body, agent_content);
        json_builder_end_object(body);
      }
    }
  }

  /* Add messages from history */
  if (json_object_has_member(obj, "messages")) {
    JsonArray *messages = json_object_get_array_member(obj, "messages");

    if (messages != NULL) {
      for (guint i = 0; i < json_array_get_length(messages); i++) {
        JsonNode *msg_node = json_array_get_element(messages, i);

        if (msg_node != NULL && !JSON_NODE_HOLDS_VALUE(msg_node))
          json_builder_add_value(body, json_node_copy(msg_node));
      }
    }
  }

  json_builder_end_array(body);
  json_builder_end_object(body);

  body_node = json_builder_get_root(body);
  {
    g_autoptr(JsonGenerator) gen = json_generator_new();
    gsize data_len;

    json_generator_set_root(gen, body_node);
    json_generator_set_pretty(gen, FALSE);
    body_json = json_generator_to_data(gen, &data_len);
    body_len = data_len;
  }
  g_object_unref(body);

  /* Build HTTP headers */
  auth_header = g_strdup_printf("Authorization: Bearer %s", state->api_key);
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_header);

  /* Build URL */
  url = g_strdup_printf("%s/chat/completions", state->base_url);

  /* Set up curl easy handle */
  state->easy = curl_easy_init();
  curl_easy_setopt(state->easy, CURLOPT_URL, url);
  curl_easy_setopt(state->easy, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(state->easy, CURLOPT_POSTFIELDS, body_json);
  curl_easy_setopt(state->easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
  curl_easy_setopt(state->easy, CURLOPT_WRITEFUNCTION, on_http_data);
  curl_easy_setopt(state->easy, CURLOPT_WRITEDATA, state);
  curl_easy_setopt(state->easy, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(state->easy, CURLOPT_USERAGENT, "promptr-openrouter/1.0");

  /* Add to multi handle */
  state->multi = curl_multi_init();
  curl_multi_add_handle(state->multi, state->easy);

  /* Register idle handler for processing curl events */
  state->curl_idle_id = g_idle_add(on_curl_idle, state);

  curl_slist_free_all(headers);

  return TRUE;
}

static gboolean handle_cancel(ProviderState *state) {
  state->cancelled = TRUE;
  if (state->active_tab_id != NULL)
    write_error_event(state->active_tab_id, "");
  return TRUE;
}

/* ── main ───────────────────────────────────────────────────────── */

static gboolean on_stdin_ready(GIOChannel *source, GIOCondition cond,
                               gpointer user_data) {
  ProviderState *state = user_data;
  g_autofree char *line = NULL;
  gsize len;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  const char *method;
  JsonNode *params;
  int id;

  if (cond & (G_IO_HUP | G_IO_ERR)) {
    g_main_loop_quit(g_main_loop_new(NULL, FALSE));
    return G_SOURCE_REMOVE;
  }

  {
    GIOStatus status;

    status = g_io_channel_read_line(source, &line, &len, NULL, &error);
    if (status != G_IO_STATUS_NORMAL) {
      g_main_loop_quit(g_main_loop_new(NULL, FALSE));
      return G_SOURCE_REMOVE;
    }
  }

  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    line[--len] = '\0';

  if (line[0] == '\0')
    return G_SOURCE_CONTINUE;

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, line, -1, &error))
    return G_SOURCE_CONTINUE;

  root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
    return G_SOURCE_CONTINUE;

  method = json_rpc_get_method(root);
  params = json_rpc_get_params(root);
  id = json_rpc_get_id(root);

  if (method != NULL && g_strcmp0(method, "provider/shutdown") == 0) {
    g_main_loop_quit(g_main_loop_new(NULL, FALSE));
    return G_SOURCE_REMOVE;
  }

  if (method != NULL && id >= 0) {
    gboolean handled = TRUE;

    if (g_strcmp0(method, "provider/initialize") == 0) {
      g_autoptr(JsonBuilder) caps = NULL;
      g_autoptr(JsonNode) caps_node = NULL;
      g_autoptr(JsonNode) resp = NULL;

      if (!handle_initialize(state, params)) {
        g_autoptr(JsonNode) err =
            json_rpc_make_error(-32010, "API key not configured", id);
        json_rpc_write(stdout, err, NULL);
        fflush(stdout);
        return G_SOURCE_CONTINUE;
      }

      caps = json_builder_new();
      json_builder_begin_object(caps);
      json_builder_set_member_name(caps, "protocol_version");
      json_builder_add_string_value(caps, "1.0");
      json_builder_set_member_name(caps, "server_capabilities");
      json_builder_begin_object(caps);
      json_builder_set_member_name(caps, "streaming");
      json_builder_add_boolean_value(caps, TRUE);
      json_builder_set_member_name(caps, "tools");
      json_builder_add_boolean_value(caps, FALSE);
      json_builder_set_member_name(caps, "agent_mode");
      json_builder_add_string_value(caps, "content");
      json_builder_end_object(caps);
      json_builder_end_object(caps);

      caps_node = json_builder_get_root(caps);
      resp = json_rpc_make_response(caps_node, id);
      json_rpc_write(stdout, resp, NULL);
      fflush(stdout);
      return G_SOURCE_CONTINUE;
    }

    if (g_strcmp0(method, "provider/submit") == 0) {
      handled = handle_submit(state, params);
    } else if (g_strcmp0(method, "provider/cancel") == 0) {
      handled = handle_cancel(state);
    } else {
      g_autoptr(JsonNode) err =
          json_rpc_make_error(-32601, "Method not found", id);
      json_rpc_write(stdout, err, NULL);
      fflush(stdout);
      return G_SOURCE_CONTINUE;
    }

    if (handled) {
      g_autoptr(JsonNode) resp = json_rpc_make_ok(id);
      json_rpc_write(stdout, resp, NULL);
      fflush(stdout);
    } else {
      g_autoptr(JsonNode) err =
          json_rpc_make_error(-32000, "Handler failed", id);
      json_rpc_write(stdout, err, NULL);
      fflush(stdout);
    }
  }

  return G_SOURCE_CONTINUE;
}

int main(void) {
  ProviderState state;
  GIOChannel *stdin_channel;
  GMainLoop *loop;

  memset(&state, 0, sizeof(state));

  setvbuf(stdout, NULL, _IONBF, 0);
  curl_global_init(CURL_GLOBAL_ALL);

  stdin_channel = g_io_channel_unix_new(STDIN_FILENO);
  g_io_channel_set_encoding(stdin_channel, NULL, NULL);
  g_io_add_watch(stdin_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin_ready,
                 &state);
  g_io_channel_unref(stdin_channel);

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);

  if (state.multi != NULL) {
    curl_multi_cleanup(state.multi);
    state.multi = NULL;
  }
  if (state.easy != NULL) {
    curl_easy_cleanup(state.easy);
    state.easy = NULL;
  }
  if (state.curl_idle_id > 0)
    g_source_remove(state.curl_idle_id);
  g_free(state.api_key);
  g_free(state.base_url);
  g_free(state.active_tab_id);
  g_free(state.active_model);
  g_free(state.full_output);
  if (state.sse_buffer != NULL)
    g_string_free(state.sse_buffer, TRUE);

  curl_global_cleanup();
  return 0;
}
