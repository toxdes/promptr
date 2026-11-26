#include "provider-server.h"
#include <signal.h>
#include <string.h>

typedef struct {
  const ProviderBackend *backend;
  GMutex output_lock;
  GMutex state_lock;
  JsonObject *settings;
  JsonObject *request;
  GCancellable *cancel;
  GThread *thread;
  gboolean busy;
  gboolean discovery;
  int id;
} Server;

void provider_digest_message(GChecksum *checksum, const char *role,
                             const char *text) {
  g_checksum_update(checksum, (const guchar *)role, strlen(role) + 1);
  g_checksum_update(checksum, (const guchar *)text, strlen(text) + 1);
}

char *provider_history_digest(JsonArray *messages, guint count) {
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  for (guint i = 0; i < MIN(count, json_array_get_length(messages)); i++) {
    JsonObject *m = json_array_get_object_element(messages, i);
    provider_digest_message(sum, protocol_string(m, "role"),
                            protocol_string(m, "content"));
  }
  return g_strdup(g_checksum_get_string(sum));
}

static gboolean send_node(Server *s, JsonNode *node, GError **error) {
  g_mutex_lock(&s->output_lock);
  gboolean ok = json_rpc_write(stdout, node, error);
  g_mutex_unlock(&s->output_lock);
  return ok;
}

static void send_error(Server *s, int id, int code, const char *message) {
  g_autoptr(JsonNode) node = json_rpc_make_error(code, message, id);
  send_node(s, node, NULL);
}

static gboolean emit_text(const char *text, gpointer data, GError **error) {
  Server *s = data;
  gsize length = strlen(text), offset = 0;
  while (offset < length) {
    gsize n = MIN((gsize)PROMPTR_MAX_DELTA, length - offset);
    while (n > 0 && !g_utf8_validate(text + offset, n, NULL))
      n--;
    if (n == 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Backend returned invalid UTF-8");
      return FALSE;
    }
    g_autoptr(JsonObject) p = json_object_new();
    json_object_set_string_member(p, "thread_id",
                                  protocol_string(s->request, "thread_id"));
    json_object_set_string_member(p, "request_id",
                                  protocol_string(s->request, "request_id"));
    json_object_set_string_member(p, "type", "delta");
    g_autofree char *part = g_strndup(text + offset, n);
    json_object_set_string_member(p, "output", part);
    g_autoptr(JsonNode) params = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(params, p);
    g_autoptr(JsonNode) node =
        json_rpc_make_notification("provider/event", g_steal_pointer(&params));
    if (!send_node(s, node, error))
      return FALSE;
    offset += n;
  }
  return TRUE;
}

static gpointer work(gpointer data) {
  Server *s = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *state = NULL;
  g_auto(GStrv) models = NULL;
  gboolean ok;
  if (s->discovery) {
    models = s->backend->models != NULL
                 ? s->backend->models(s->settings, s->cancel, &error)
                 : NULL;
    ok = models != NULL;
    if (!ok && error == NULL)
      g_set_error_literal(
          &error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "Model discovery is unavailable; enter a model ID manually");
  } else {
    ok = s->backend->submit(s->settings, s->request, s->cancel, emit_text, s,
                            &state, &error);
  }
  gboolean cancelled = g_cancellable_is_cancelled(s->cancel);
  g_mutex_lock(&s->state_lock);
  if (!ok && !cancelled)
    send_error(s, s->id, -32000,
               error != NULL ? error->message : "Backend request failed");
  else {
    g_autoptr(JsonObject) result = json_object_new();
    if (s->discovery) {
      JsonArray *array = json_array_new();
      for (guint i = 0; models != NULL && models[i] != NULL; i++)
        json_array_add_string_element(array, models[i]);
      json_object_set_array_member(result, "models", array);
    } else {
      json_object_set_string_member(result, "thread_id",
                                    protocol_string(s->request, "thread_id"));
      json_object_set_string_member(result, "request_id",
                                    protocol_string(s->request, "request_id"));
      json_object_set_string_member(result, "status",
                                    cancelled ? "cancelled" : "completed");
      if (state != NULL && !cancelled)
        json_object_set_string_member(result, "resume_state", state);
    }
    g_autoptr(JsonNode) p = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(p, result);
    g_autoptr(JsonNode) response =
        json_rpc_make_response(g_steal_pointer(&p), s->id);
    send_node(s, response, NULL);
  }
  s->busy = FALSE;
  g_mutex_unlock(&s->state_lock);
  return NULL;
}

static gboolean valid_submit(JsonObject *p) {
  if (protocol_string(p, "thread_id") == NULL ||
      protocol_string(p, "request_id") == NULL)
    return FALSE;
  const char *ids[] = {protocol_string(p, "thread_id"),
                       protocol_string(p, "request_id")};
  for (guint i = 0; i < G_N_ELEMENTS(ids); i++)
    if (!*ids[i] || strlen(ids[i]) > 256)
      return FALSE;
  const char *strings[] = {"model", "project_dir", "resume_state"};
  for (guint i = 0; i < G_N_ELEMENTS(strings); i++)
    if (json_object_has_member(p, strings[i]) &&
        protocol_string(p, strings[i]) == NULL)
      return FALSE;
  const char *project = protocol_string(p, "project_dir");
  if (project != NULL && *project && !g_path_is_absolute(project))
    return FALSE;
  JsonNode *test = json_object_get_member(p, "connection_test");
  if (test != NULL && (!JSON_NODE_HOLDS_VALUE(test) ||
                       json_node_get_value_type(test) != G_TYPE_BOOLEAN))
    return FALSE;
  JsonNode *list = json_object_get_member(p, "messages");
  if (list == NULL || !JSON_NODE_HOLDS_ARRAY(list))
    return FALSE;
  JsonArray *a = json_node_get_array(list);
  if (json_array_get_length(a) == 0)
    return FALSE;
  for (guint i = 0; i < json_array_get_length(a); i++) {
    JsonNode *n = json_array_get_element(a, i);
    if (!JSON_NODE_HOLDS_OBJECT(n))
      return FALSE;
    JsonObject *m = json_node_get_object(n);
    const char *role = protocol_string(m, "role");
    if (protocol_string(m, "content") == NULL ||
        (g_strcmp0(role, "user") != 0 && g_strcmp0(role, "assistant") != 0 &&
         g_strcmp0(role, "system") != 0))
      return FALSE;
  }
  JsonObject *last =
      json_array_get_object_element(a, json_array_get_length(a) - 1);
  if (g_strcmp0(protocol_string(last, "role"), "user") != 0)
    return FALSE;
  if (json_object_has_member(p, "agent") &&
      protocol_string(protocol_object(p, "agent"), "content") == NULL)
    return FALSE;
  return TRUE;
}

int provider_server_run(const ProviderBackend *backend) {
  signal(SIGPIPE, SIG_IGN);
  Server s = {.backend = backend};
  g_mutex_init(&s.output_lock);
  g_mutex_init(&s.state_lock);
  gboolean initialized = FALSE;
  int status = 0;
  for (;;) {
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = json_rpc_read(stdin, &error);
    if (node == NULL) {
      if (error != NULL)
        status = 1;
      break;
    }
    const char *method = json_rpc_get_method(node);
    int id = json_rpc_get_id(node);
    if (method == NULL || id < 0) {
      send_error(&s, MAX(id, 0), -32600, "Expected a request with integer ID");
      continue;
    }
    JsonObject *p = protocol_object(json_node_get_object(node), "params");
    g_mutex_lock(&s.state_lock);
    gboolean busy = s.busy;
    g_mutex_unlock(&s.state_lock);
    if (g_strcmp0(method, "provider/cancel") == 0) {
      if (busy &&
          g_strcmp0(protocol_string(p, "thread_id"),
                    protocol_string(s.request, "thread_id")) == 0 &&
          g_strcmp0(protocol_string(p, "request_id"),
                    protocol_string(s.request, "request_id")) == 0)
        g_cancellable_cancel(s.cancel);
      g_autoptr(JsonNode) ack = json_rpc_make_ok(id);
      send_node(&s, ack, NULL);
      continue;
    }
    if (g_strcmp0(method, "provider/shutdown") == 0)
      break;
    if (busy) {
      send_error(&s, id, -32020, "Provider is busy");
      continue;
    }
    if (s.thread != NULL) {
      g_thread_join(s.thread);
      s.thread = NULL;
    }
    if (g_strcmp0(method, "provider/initialize") == 0) {
      if (initialized) {
        send_error(&s, id, -32600, "Provider is already initialized");
        continue;
      }
      if (g_strcmp0(protocol_string(p, "protocol_version"),
                    PROMPTR_PROTOCOL_VERSION) != 0) {
        send_error(&s, id, -32001,
                   "This plugin requires Promptr provider protocol 2.0");
        continue;
      }
      JsonObject *settings = protocol_object(p, "config");
      if (settings == NULL) {
        send_error(&s, id, -32602, "config must be an object");
        continue;
      }
      if (backend->initialize != NULL &&
          !backend->initialize(
              settings, g_strcmp0(protocol_string(p, "purpose"), "models") == 0,
              &error)) {
        send_error(&s, id, -32010,
                   error != NULL ? error->message : "Invalid configuration");
        continue;
      }
      s.settings = json_object_ref(settings);
      initialized = TRUE;
      g_autoptr(JsonObject) result = json_object_new();
      json_object_set_string_member(result, "protocol_version",
                                    PROMPTR_PROTOCOL_VERSION);
      JsonObject *caps = json_object_new();
      json_object_set_boolean_member(caps, "streaming", TRUE);
      json_object_set_boolean_member(caps, "models", backend->models != NULL);
      json_object_set_object_member(result, "capabilities", caps);
      g_autoptr(JsonNode) r = json_node_new(JSON_NODE_OBJECT);
      json_node_set_object(r, result);
      g_autoptr(JsonNode) response =
          json_rpc_make_response(g_steal_pointer(&r), id);
      send_node(&s, response, NULL);
    } else if (!initialized)
      send_error(&s, id, -32600, "Initialize the provider first");
    else if (g_strcmp0(method, "provider/submit") == 0 ||
             g_strcmp0(method, "provider/models") == 0) {
      s.discovery = g_strcmp0(method, "provider/models") == 0;
      if (!s.discovery && !valid_submit(p)) {
        send_error(&s, id, -32602, "Invalid messages or request identifiers");
        continue;
      }
      if (s.request != NULL)
        json_object_unref(s.request);
      s.request = p != NULL ? json_object_ref(p) : json_object_new();
      g_clear_object(&s.cancel);
      s.cancel = g_cancellable_new();
      s.id = id;
      g_mutex_lock(&s.state_lock);
      s.busy = TRUE;
      g_mutex_unlock(&s.state_lock);
      s.thread = g_thread_new("provider-request", work, &s);
    } else
      send_error(&s, id, -32601, "Method not supported");
  }
  if (s.cancel != NULL)
    g_cancellable_cancel(s.cancel);
  if (s.thread != NULL)
    g_thread_join(s.thread);
  g_clear_object(&s.cancel);
  if (s.settings != NULL)
    json_object_unref(s.settings);
  if (s.request != NULL)
    json_object_unref(s.request);
  g_mutex_clear(&s.output_lock);
  g_mutex_clear(&s.state_lock);
  return status;
}
