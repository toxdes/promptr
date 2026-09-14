#include "plugin-runner.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "configfile.h"
#include "provider.h"
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Private state ─────────────────────────────────────────────── */

typedef struct {
  char *command;
  char *tab_key; /* current active tab key string */
  gboolean has_session;

  /* subprocess handles */
  GPid child_pid;
  int stdin_fd;
  int stdout_fd;
  guint stdout_watch;
  GString *read_buf;

  /* callback state */
  ProviderCallback callback;
  gpointer callback_data;
  gpointer active_tab;
  gint64 submit_start;
  gboolean submitting;

  /* JSON-RPC */
  int next_id;
} PluginRunner;

/* ── Forward declarations ──────────────────────────────────────── */

static gboolean runner_init(void **impl, RuntimeConfig *config, GError **error);
static void runner_destroy(void *impl);
static void runner_cleanup_session(void *impl, gpointer tab_key);
static gboolean runner_submit(void *impl, gpointer tab_key, const char *query,
                              const ProviderMessage *history, int n_history,
                              const char *model, const char *agent,
                              gboolean is_follow_up, GCancellable *cancellable,
                              ProviderCallback callback, gpointer callback_data,
                              GError **error);
static void runner_cancel(void *impl);
static void runner_cancel_tab(void *impl, gpointer tab_key);
static char *runner_get_display_string(void *impl, const char *query,
                                       const char *model, const char *agent,
                                       gboolean is_follow_up);
static char **runner_get_models(void *impl);
static char **runner_get_agents(void *impl);

/* ── VTable ────────────────────────────────────────────────────── */

static ProviderVTable plugin_runner_vtable = {
    .name = "plugin",
    .init = runner_init,
    .destroy = runner_destroy,
    .cleanup_session = runner_cleanup_session,
    .submit = runner_submit,
    .cancel = runner_cancel,
    .cancel_tab = runner_cancel_tab,
    .get_display_string = runner_get_display_string,
    .get_models = runner_get_models,
    .get_agents = runner_get_agents,
    .needs_full_history = TRUE,
};

/* ── Internal helpers ──────────────────────────────────────────── */

static void plugin_runner_free(PluginRunner *pr) {
  if (pr == NULL)
    return;

  g_free(pr->command);
  g_free(pr->tab_key);
  if (pr->read_buf != NULL)
    g_string_free(pr->read_buf, TRUE);
  g_free(pr);
}

static gboolean stdin_write(PluginRunner *pr, const char *data) {
  const char *p = data;
  size_t remaining = strlen(data);
  ssize_t written;

  while (remaining > 0) {
    written = write(pr->stdin_fd, p, remaining);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return FALSE;
    }
    p += written;
    remaining -= (size_t)written;
  }

  return TRUE;
}

/* Write a message + newline to the plugin's stdin. */
static gboolean protocol_write(PluginRunner *pr, JsonNode *msg) {
  g_autoptr(JsonGenerator) gen = NULL;
  g_autofree char *json = NULL;
  gsize len;
  gboolean ok;

  gen = json_generator_new();
  json_generator_set_root(gen, msg);
  json_generator_set_pretty(gen, FALSE);
  json = json_generator_to_data(gen, &len);

  ok = stdin_write(pr, json);
  if (ok)
    ok = stdin_write(pr, "\n");

  return ok;
}

/* Blocking read of one JSON-RPC response line from stdout. */
static JsonNode *read_response(PluginRunner *pr, GError **error) {
  g_autofree char *line = NULL;
  gsize len;
  g_autoptr(JsonParser) parser = NULL;
  GIOChannel *channel;
  GIOStatus status;

  channel = g_io_channel_unix_new(pr->stdout_fd);
  g_io_channel_set_encoding(channel, NULL, NULL);

  while (TRUE) {
    g_free(line);
    line = NULL;
    len = 0;

    status = g_io_channel_read_line(channel, &line, &len, NULL, error);

    if (status != G_IO_STATUS_NORMAL) {
      if (status == G_IO_STATUS_EOF)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "plugin closed stdout unexpectedly");
      g_io_channel_unref(channel);
      return NULL;
    }

    /* Strip trailing newline */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if (line[0] == '\0')
      continue;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, error)) {
      g_io_channel_unref(channel);
      return NULL;
    }

    g_io_channel_unref(channel);
    return json_node_copy(json_parser_get_root(parser));
  }
}

/* ── Initialize handshake ───────────────────────────────────────── */

static gboolean do_initialize(PluginRunner *pr, RuntimeConfig *config,
                              GError **error) {
  (void)config;
  g_autoptr(JsonNode) req = NULL;
  g_autoptr(JsonNode) resp = NULL;
  g_autoptr(JsonBuilder) params = NULL;
  g_autoptr(JsonBuilder) caps = NULL;
  g_autofree char *provider_name = NULL;

  params = json_builder_new();
  json_builder_begin_object(params);
  json_builder_set_member_name(params, "protocol_version");
  json_builder_add_string_value(params, "1.0");
  json_builder_set_member_name(params, "client_capabilities");
  json_builder_begin_object(params);
  json_builder_set_member_name(params, "tool_execution");
  json_builder_add_boolean_value(params, TRUE);
  json_builder_set_member_name(params, "agent_content");
  json_builder_add_boolean_value(params, TRUE);
  json_builder_set_member_name(params, "agent_resolution");
  json_builder_add_boolean_value(params, TRUE);
  json_builder_set_member_name(params, "streaming");
  json_builder_add_boolean_value(params, TRUE);
  json_builder_end_object(params);
  json_builder_end_object(params);

  {
    JsonNode *params_node = json_builder_get_root(params);
    req = json_rpc_make_request("provider/initialize", params_node,
                                pr->next_id++);
  }

  if (!protocol_write(pr, req)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to write initialize request");
    return FALSE;
  }

  resp = read_response(pr, error);
  if (resp == NULL)
    return FALSE;

  if (json_rpc_is_error(resp)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Plugin initialize error: %s",
                json_rpc_get_error_message(resp));
    return FALSE;
  }

  return TRUE;
}

/* ── Spawn ──────────────────────────────────────────────────────── */

static gboolean spawn_plugin(PluginRunner *pr, GError **error) {
  const char *argv[2] = {pr->command, NULL};
  g_autoptr(GError) spawn_err = NULL;

  if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pr->child_pid, &pr->stdin_fd,
                                &pr->stdout_fd, NULL, &spawn_err)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Failed to spawn plugin '%s': %s", pr->command,
                spawn_err->message);
    return FALSE;
  }

  return TRUE;
}

/* ── Async stdout reader ────────────────────────────────────────── */

static void dispatch_event(PluginRunner *pr, const char *tab_id,
                           const char *type, JsonNode *params) {
  (void)tab_id;
  ProviderEvent ev;
  gint64 elapsed = g_get_monotonic_time() - pr->submit_start;

  memset(&ev, 0, sizeof(ev));

  if (g_strcmp0(type, "chunk") == 0) {
    const char *output = NULL;

    if (json_object_has_member(json_node_get_object(params), "output"))
      output =
          json_object_get_string_member(json_node_get_object(params), "output");
    ev.type = PROVIDER_EVENT_CHUNK;
    ev.output = (char *)(output != NULL ? output : "");
    ev.elapsed_us = elapsed;
    pr->callback(pr->active_tab, &ev, pr->callback_data);
    return;
  }

  if (g_strcmp0(type, "done") == 0) {
    const char *output = NULL;

    if (json_object_has_member(json_node_get_object(params), "output"))
      output =
          json_object_get_string_member(json_node_get_object(params), "output");
    ev.type = PROVIDER_EVENT_DONE;
    ev.output = (char *)(output != NULL ? output : "");
    ev.elapsed_us = elapsed;
    pr->callback(pr->active_tab, &ev, pr->callback_data);
    pr->submitting = FALSE;
    return;
  }

  if (g_strcmp0(type, "error") == 0) {
    const char *err_msg = NULL;

    if (json_object_has_member(json_node_get_object(params), "error_msg"))
      err_msg = json_object_get_string_member(json_node_get_object(params),
                                              "error_msg");
    ev.type = PROVIDER_EVENT_ERROR;
    ev.error_msg = (char *)(err_msg != NULL ? err_msg : "");
    ev.elapsed_us = elapsed;
    pr->callback(pr->active_tab, &ev, pr->callback_data);
    pr->submitting = FALSE;
    return;
  }
}

static void process_lines(PluginRunner *pr) {
  char *data = pr->read_buf->str;

  while (TRUE) {
    char *newline = strchr(data, '\n');

    if (newline == NULL)
      break;

    *newline = '\0';

    if (data[0] != '\0' && pr->callback != NULL) {
      g_autoptr(JsonParser) parser = NULL;
      g_autoptr(GError) err = NULL;
      JsonNode *root;
      JsonObject *obj;
      const char *method;
      JsonNode *params;

      parser = json_parser_new();
      if (json_parser_load_from_data(parser, data, -1, &err)) {
        root = json_parser_get_root(parser);
        if (root != NULL && !JSON_NODE_HOLDS_VALUE(root)) {
          obj = json_node_get_object(root);
          method = json_rpc_get_method(root);
          params = json_rpc_get_params(root);

          /* Only handle event notifications during submit */
          if (method != NULL && g_strcmp0(method, "provider/event") == 0 &&
              params != NULL) {
            const char *tab_id = NULL;
            const char *type = NULL;

            if (json_object_has_member(obj, "tab_id"))
              tab_id = json_object_get_string_member(obj, "tab_id");
            if (json_object_has_member(obj, "type"))
              type = json_object_get_string_member(obj, "type");

            if (tab_id == NULL &&
                json_object_has_member(json_node_get_object(params), "tab_id"))
              tab_id = json_object_get_string_member(
                  json_node_get_object(params), "tab_id");
            if (type == NULL &&
                json_object_has_member(json_node_get_object(params), "type"))
              type = json_object_get_string_member(json_node_get_object(params),
                                                   "type");

            if (tab_id != NULL && type != NULL)
              dispatch_event(pr, tab_id, type, params);
          }
        }
      }
    }

    data = newline + 1;
  }

  /* Keep remaining partial data */
  if (data > pr->read_buf->str) {
    GString *remaining = g_string_new(data);

    g_string_free(pr->read_buf, TRUE);
    pr->read_buf = remaining;
  }
}

static gboolean on_stdout_ready(GIOChannel *source, GIOCondition cond,
                                gpointer user_data) {
  PluginRunner *pr = user_data;
  char buf[4096];
  gsize bytes_read;
  g_autoptr(GError) error = NULL;

  if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
    g_warning("plugin-runner: stdout closed or error (cond=0x%x)",
              (unsigned)cond);
    if (pr->submitting && pr->callback != NULL) {
      ProviderEvent ev = {PROVIDER_EVENT_ERROR, NULL, (char *)"", 0};

      pr->callback(pr->active_tab, &ev, pr->callback_data);
      pr->submitting = FALSE;
    }
    return G_SOURCE_REMOVE;
  }

  if (!g_io_channel_read_chars(source, buf, sizeof(buf) - 1, &bytes_read,
                               &error)) {
    g_warning("plugin-runner: read error: %s", error->message);
    return G_SOURCE_CONTINUE;
  }

  if (bytes_read > 0) {
    buf[bytes_read] = '\0';
    g_string_append_len(pr->read_buf, buf, bytes_read);
    process_lines(pr);
  }

  return G_SOURCE_CONTINUE;
}

static void setup_stdout_watch(PluginRunner *pr) {
  GIOChannel *channel;

  channel = g_io_channel_unix_new(pr->stdout_fd);
  g_io_channel_set_encoding(channel, NULL, NULL);
  pr->stdout_watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    on_stdout_ready, pr);
  g_io_channel_unref(channel);
}

/* ── VTable implementations ────────────────────────────────────── */

Provider *plugin_runner_create(const char *command, RuntimeConfig *config,
                               GError **error) {
  PluginRunner *pr;

  g_return_val_if_fail(command != NULL, NULL);
  g_return_val_if_fail(config != NULL, NULL);

  pr = g_new0(PluginRunner, 1);
  pr->command = g_strdup(command);
  pr->next_id = 1;
  pr->read_buf = g_string_new(NULL);

  if (!spawn_plugin(pr, error))
    goto fail;

  setup_stdout_watch(pr);

  if (!do_initialize(pr, config, error))
    goto fail;

  {
    Provider *p = g_new0(Provider, 1);

    p->vtable = &plugin_runner_vtable;
    p->impl = pr;
    return p;
  }

fail:
  if (pr->stdout_watch > 0)
    g_source_remove(pr->stdout_watch);
  if (pr->stdout_fd >= 0)
    close(pr->stdout_fd);
  if (pr->stdin_fd >= 0)
    close(pr->stdin_fd);
  if (pr->child_pid > 0)
    g_spawn_close_pid(pr->child_pid);
  plugin_runner_free(pr);
  return NULL;
}

static gboolean runner_init(void **impl, RuntimeConfig *config,
                            GError **error) {
  (void)impl;
  (void)config;
  (void)error;
  return TRUE;
}

static void runner_destroy(void *impl) {
  PluginRunner *pr = impl;

  if (pr == NULL)
    return;

  /* Send shutdown, then close stdin and clean up */
  {
    g_autoptr(JsonNode) req =
        json_rpc_make_request("provider/shutdown", NULL, pr->next_id++);
    g_autofree char *json = NULL;
    g_autoptr(JsonGenerator) gen = NULL;
    gsize len;

    gen = json_generator_new();
    json_generator_set_root(gen, req);
    json_generator_set_pretty(gen, FALSE);
    json = json_generator_to_data(gen, &len);

    if (pr->stdin_fd >= 0) {
      stdin_write(pr, json);
      stdin_write(pr, "\n");
    }
  }

  if (pr->stdout_watch > 0)
    g_source_remove(pr->stdout_watch);

  if (pr->stdin_fd >= 0)
    close(pr->stdin_fd);
  if (pr->stdout_fd >= 0)
    close(pr->stdout_fd);

  if (pr->child_pid > 0) {
    g_spawn_close_pid(pr->child_pid);
    pr->child_pid = 0;
  }

  plugin_runner_free(pr);
}

static void runner_cleanup_session(void *impl, gpointer tab_key) {
  PluginRunner *pr = impl;

  (void)pr;
  (void)tab_key;
  /* Plugin manages per-tab state via tab_id internally.
     Promptr sends provider/cleanup_session via the protocol
     when needed. No-op for now. */
}

static gboolean runner_submit(void *impl, gpointer tab_key, const char *query,
                              const ProviderMessage *history, int n_history,
                              const char *model, const char *agent,
                              gboolean is_follow_up, GCancellable *cancellable,
                              ProviderCallback callback, gpointer callback_data,
                              GError **error) {
  PluginRunner *pr = impl;
  g_autoptr(JsonBuilder) params = NULL;
  g_autoptr(JsonBuilder) msg_builder = NULL;
  g_autoptr(JsonNode) params_node = NULL;
  g_autoptr(JsonNode) req = NULL;
  const char *tab_id_str;
  g_autofree char *auto_tab_id = NULL;

  (void)cancellable;
  (void)error;

  auto_tab_id = g_strdup_printf("tab_%p", tab_key);
  tab_id_str = auto_tab_id;

  pr->callback = callback;
  pr->callback_data = callback_data;
  pr->active_tab = tab_key;
  pr->submit_start = g_get_monotonic_time();
  pr->submitting = TRUE;

  params = json_builder_new();
  json_builder_begin_object(params);

  json_builder_set_member_name(params, "tab_id");
  json_builder_add_string_value(params, tab_id_str);

  json_builder_set_member_name(params, "query");
  json_builder_add_string_value(params, query != NULL ? query : "");

  json_builder_set_member_name(params, "is_follow_up");
  json_builder_add_boolean_value(params, is_follow_up);

  if (model != NULL && model[0] != '\0' && g_strcmp0(model, "None") != 0) {
    json_builder_set_member_name(params, "model");
    json_builder_add_string_value(params, model);
  }

  /* Build messages array from history */
  json_builder_set_member_name(params, "messages");
  json_builder_begin_array(params);
  for (int i = 0; i < n_history; i++) {
    const char *role_str;

    if (history[i].content == NULL || history[i].content[0] == '\0')
      continue;

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

    json_builder_begin_object(params);
    json_builder_set_member_name(params, "role");
    json_builder_add_string_value(params, role_str);
    json_builder_set_member_name(params, "content");
    json_builder_add_string_value(params, history[i].content);
    json_builder_end_object(params);
  }
  json_builder_end_array(params);

  /* Agent block */
  if (agent != NULL && agent[0] != '\0' && g_strcmp0(agent, "None") != 0) {
    json_builder_set_member_name(params, "agent");
    json_builder_begin_object(params);
    json_builder_set_member_name(params, "name");
    json_builder_add_string_value(params, agent);
    json_builder_set_member_name(params, "content");
    json_builder_add_string_value(params, agent);
    json_builder_end_object(params);
  }

  json_builder_end_object(params);

  params_node = json_builder_get_root(params);
  req = json_rpc_make_request("provider/submit", params_node, pr->next_id++);

  if (!protocol_write(pr, req)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to write submit request");
    pr->submitting = FALSE;
    return FALSE;
  }

  return TRUE;
}

static void runner_cancel(void *impl) {
  PluginRunner *pr = impl;

  if (!pr->submitting)
    return;

  {
    g_autoptr(JsonNode) req =
        json_rpc_make_request("provider/cancel", NULL, pr->next_id++);

    protocol_write(pr, req);
  }
}

static void runner_cancel_tab(void *impl, gpointer tab_key) {
  (void)impl;
  (void)tab_key;
  runner_cancel(impl);
}

static char *runner_get_display_string(void *impl, const char *query,
                                       const char *model, const char *agent,
                                       gboolean is_follow_up) {
  PluginRunner *pr = impl;
  GString *s;

  (void)agent;
  (void)is_follow_up;

  s = g_string_new(pr->command);
  if (model != NULL && model[0] != '\0' && g_strcmp0(model, "None") != 0)
    g_string_append_printf(s, " [%s]", model);
  g_string_append_printf(s, " %s", query != NULL ? query : "");

  return g_string_free(s, FALSE);
}

static char **runner_get_models(void *impl) {
  (void)impl;
  return NULL;
}

static char **runner_get_agents(void *impl) {
  (void)impl;
  return NULL;
}
