#include "../../lib/promptr-protocol/provider-server.h"
#include <curl/curl.h>
#include <string.h>

typedef struct {
  GCancellable *cancel;
  ProviderEmit emit;
  gpointer emit_data;
  GString *buffer;
  GString *body;
  GError *error;
  gboolean done;
} Transfer;

static const char *setting(JsonObject *settings, const char *key,
                           const char *env, const char *fallback) {
  const char *value = protocol_string(settings, key);
  if (value == NULL || !*value)
    value = env != NULL ? g_getenv(env) : NULL;
  return value != NULL && *value ? value : fallback;
}

static gboolean initialize(JsonObject *settings, gboolean discovery,
                           GError **error) {
  if (!discovery &&
      setting(settings, "api_key", "OPENROUTER_API_KEY", NULL) == NULL) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "No OpenRouter API key configured. Open Preferences, "
        "select OpenRouter and enter your API key, or set OPENROUTER_API_KEY.");
    return FALSE;
  }
  const char *url =
      setting(settings, "base_url", NULL, "https://openrouter.ai/api/v1");
  if (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://")) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Endpoint must start with https:// or http://");
    return FALSE;
  }
  return TRUE;
}

static int progress(void *data, curl_off_t a, curl_off_t b, curl_off_t c,
                    curl_off_t d) {
  Transfer *t = data;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return g_cancellable_is_cancelled(t->cancel);
}

static size_t receive(char *data, size_t size, size_t count, void *user_data) {
  Transfer *t = user_data;
  gsize len = size * count;
  if (t->emit == NULL) {
    if (t->body->len + len > 8 * 1024 * 1024)
      return 0;
    g_string_append_len(t->body, data, len);
    return len;
  }
  if (t->buffer->len + len > 1024 * 1024)
    return 0;
  g_string_append_len(t->buffer, data, len);
  for (;;) {
    char *newline = strchr(t->buffer->str, '\n');
    if (newline == NULL)
      break;
    gsize length = newline - t->buffer->str;
    g_autofree char *line = g_strndup(t->buffer->str, length);
    g_string_erase(t->buffer, 0, length + 1);
    g_strchomp(line);
    if (!g_str_has_prefix(line, "data:"))
      continue;
    const char *payload = line + 5;
    while (*payload == ' ')
      payload++;
    if (g_strcmp0(payload, "[DONE]") == 0) {
      t->done = TRUE;
      continue;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, &t->error))
      return 0;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
      return 0;
    JsonObject *o = json_node_get_object(root);
    if (protocol_object(o, "error") != NULL) {
      g_set_error_literal(&t->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Backend reported a streaming error");
      return 0;
    }
    JsonNode *choices = json_object_get_member(o, "choices");
    if (choices == NULL || !JSON_NODE_HOLDS_ARRAY(choices))
      continue;
    JsonArray *array = json_node_get_array(choices);
    for (guint i = 0; i < json_array_get_length(array); i++) {
      JsonNode *choice = json_array_get_element(array, i);
      if (!JSON_NODE_HOLDS_OBJECT(choice))
        continue;
      JsonObject *delta =
          protocol_object(json_node_get_object(choice), "delta");
      const char *text = protocol_string(delta, "content");
      if (text != NULL && *text && !t->emit(text, t->emit_data, &t->error))
        return 0;
    }
  }
  return len;
}

static gboolean transfer(JsonObject *settings, const char *path,
                         const char *body, Transfer *t, GError **error) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not initialize HTTP transport");
    return FALSE;
  }
  g_autofree char *base = g_strdup(
      setting(settings, "base_url", NULL, "https://openrouter.ai/api/v1"));
  while (*base && base[strlen(base) - 1] == '/')
    base[strlen(base) - 1] = '\0';
  g_autofree char *url = g_strconcat(base, path, NULL);
  g_autofree char *auth =
      g_strconcat("Authorization: Bearer ",
                  setting(settings, "api_key", "OPENROUTER_API_KEY", ""), NULL);
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (setting(settings, "api_key", "OPENROUTER_API_KEY", NULL) != NULL)
    headers = curl_slist_append(headers, auth);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, t);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "promptr-openrouter/2.0");
  if (body != NULL) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  }
  CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (t->error != NULL) {
    g_propagate_error(error, g_steal_pointer(&t->error));
    return FALSE;
  }
  if (code != CURLE_OK || status < 200 || status >= 300) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Backend request failed (HTTP %ld, %s)", status,
                curl_easy_strerror(code));
    return FALSE;
  }
  return TRUE;
}

static gboolean submit(JsonObject *settings, JsonObject *request,
                       GCancellable *cancel, ProviderEmit emit,
                       gpointer emit_data, char **resume, GError **error) {
  (void)resume;
  if (setting(settings, "api_key", "OPENROUTER_API_KEY", NULL) == NULL) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "No OpenRouter API key configured. Open Preferences, "
        "select OpenRouter and enter your API key, or set OPENROUTER_API_KEY.");
    return FALSE;
  }
  g_autoptr(JsonObject) body = json_object_new();
  const char *model = protocol_string(request, "model");
  json_object_set_string_member(
      body, "model", model != NULL && *model ? model : "openrouter/auto");
  json_object_set_boolean_member(body, "stream", TRUE);
  if (protocol_boolean(request, "connection_test"))
    json_object_set_int_member(body, "max_tokens", 16);
  JsonArray *messages = json_array_new();
  const char *context =
      protocol_string(protocol_object(request, "agent"), "content");
  if (context != NULL && *context) {
    JsonObject *system = json_object_new();
    json_object_set_string_member(system, "role", "system");
    json_object_set_string_member(system, "content", context);
    json_array_add_object_element(messages, system);
  }
  JsonArray *history = json_object_get_array_member(request, "messages");
  for (guint i = 0; i < json_array_get_length(history); i++)
    json_array_add_element(messages,
                           json_node_copy(json_array_get_element(history, i)));
  json_object_set_array_member(body, "messages", messages);
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, body);
  g_autofree char *json = json_to_string(node, FALSE);
  g_autoptr(GString) buffer = g_string_new(NULL);
  Transfer t = {
      .cancel = cancel, .emit = emit, .emit_data = emit_data, .buffer = buffer};
  if (!transfer(settings, "/chat/completions", json, &t, error))
    return FALSE;
  if (!t.done) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Backend stream ended without a completion marker");
    return FALSE;
  }
  return TRUE;
}

static char **models(JsonObject *settings, GCancellable *cancel,
                     GError **error) {
  g_autoptr(GString) body = g_string_new(NULL);
  Transfer t = {.cancel = cancel, .body = body};
  if (!transfer(settings, "/models", NULL, &t, error))
    return NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, body->str, body->len, error))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  JsonNode *data =
      JSON_NODE_HOLDS_OBJECT(root)
          ? json_object_get_member(json_node_get_object(root), "data")
          : NULL;
  if (data == NULL || !JSON_NODE_HOLDS_ARRAY(data)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Backend returned an invalid model list");
    return NULL;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  JsonArray *array = json_node_get_array(data);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *n = json_array_get_element(array, i);
    const char *id = JSON_NODE_HOLDS_OBJECT(n)
                         ? protocol_string(json_node_get_object(n), "id")
                         : NULL;
    if (id != NULL)
      g_ptr_array_add(out, g_strdup(id));
  }
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

int main(void) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return 1;
  const ProviderBackend backend = {initialize, submit, models};
  int code = provider_server_run(&backend);
  curl_global_cleanup();
  return code;
}
