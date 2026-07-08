#include "jsonrpc.h"
#include <errno.h>
#include <string.h>

#define MAX_LINE 65536

/* ── I/O ───────────────────────────────────────────────── */

JsonNode *json_rpc_read(FILE *stream, GError **error) {
  char buf[MAX_LINE];
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root;
  JsonObject *obj;

  if (fgets(buf, (int)sizeof(buf), stream) == NULL) {
    if (feof(stream))
      return NULL;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "read: %s",
                g_strerror(errno));
    return NULL;
  }

  /* Strip trailing newline */
  {
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
      buf[--len] = '\0';
  }

  if (buf[0] == '\0')
    return json_rpc_read(stream, error);

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, buf, -1, error))
    return NULL;

  root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "JSON message must be an object");
    return NULL;
  }

  obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "jsonrpc")) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Missing 'jsonrpc' field");
    return NULL;
  }

  return json_node_copy(root);
}

gboolean json_rpc_write(FILE *stream, JsonNode *msg, GError **error) {
  g_autoptr(JsonGenerator) gen = NULL;
  g_autofree char *json = NULL;
  gsize len;

  gen = json_generator_new();
  json_generator_set_root(gen, msg);
  json_generator_set_pretty(gen, FALSE);

  json = json_generator_to_data(gen, &len);

  if (fprintf(stream, "%s\n", json) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "write: %s",
                g_strerror(errno));
    return FALSE;
  }
  fflush(stream);
  return TRUE;
}

/* ── constructors ──────────────────────────────────────── */

JsonNode *json_rpc_make_request(const char *method, JsonNode *params, int id) {
  JsonBuilder *b = json_builder_new();
  JsonNode *root;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, method);
  if (params != NULL) {
    json_builder_set_member_name(b, "params");
    json_builder_add_value(b, params);
  }
  json_builder_end_object(b);

  root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

JsonNode *json_rpc_make_response(JsonNode *result, int id) {
  JsonBuilder *b = json_builder_new();
  JsonNode *root;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "result");
  json_builder_add_value(b, result);
  json_builder_end_object(b);

  root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

JsonNode *json_rpc_make_error(int code, const char *message, int id) {
  JsonBuilder *b = json_builder_new();
  JsonNode *root;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "error");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "code");
  json_builder_add_int_value(b, code);
  json_builder_set_member_name(b, "message");
  json_builder_add_string_value(b, message);
  json_builder_end_object(b);
  json_builder_end_object(b);

  root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

JsonNode *json_rpc_make_notification(const char *method, JsonNode *params) {
  JsonBuilder *b = json_builder_new();
  JsonNode *root;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, method);
  if (params != NULL) {
    json_builder_set_member_name(b, "params");
    json_builder_add_value(b, params);
  }
  json_builder_end_object(b);

  root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

JsonNode *json_rpc_make_ok(int id) {
  JsonBuilder *b = json_builder_new();
  JsonNode *root;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "result");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);
  json_builder_end_object(b);

  root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

/* ── accessors ─────────────────────────────────────────── */

const char *json_rpc_get_method(JsonNode *msg) {
  JsonObject *obj;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return NULL;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "method"))
    return NULL;
  return json_object_get_string_member(obj, "method");
}

JsonNode *json_rpc_get_params(JsonNode *msg) {
  JsonObject *obj;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return NULL;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "params"))
    return NULL;
  return json_object_get_member(obj, "params");
}

int json_rpc_get_id(JsonNode *msg) {
  JsonObject *obj;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return -1;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "id"))
    return -1;

  {
    JsonNode *id_node = json_object_get_member(obj, "id");

    if (id_node == NULL)
      return -1;
    if (JSON_NODE_HOLDS_VALUE(id_node))
      return json_node_get_int(id_node);
    return -1;
  }
}

JsonNode *json_rpc_get_result(JsonNode *msg) {
  JsonObject *obj;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return NULL;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "result"))
    return NULL;
  return json_object_get_member(obj, "result");
}

int json_rpc_get_error_code(JsonNode *msg) {
  JsonObject *obj, *err_obj;
  JsonNode *err_node;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return 0;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "error"))
    return 0;

  err_node = json_object_get_member(obj, "error");
  if (err_node == NULL || JSON_NODE_HOLDS_VALUE(err_node))
    return 0;

  err_obj = json_node_get_object(err_node);
  if (!json_object_has_member(err_obj, "code"))
    return 0;

  return json_object_get_int_member(err_obj, "code");
}

const char *json_rpc_get_error_message(JsonNode *msg) {
  JsonObject *obj, *err_obj;
  JsonNode *err_node;

  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return NULL;
  obj = json_node_get_object(msg);
  if (!json_object_has_member(obj, "error"))
    return NULL;

  err_node = json_object_get_member(obj, "error");
  if (err_node == NULL || JSON_NODE_HOLDS_VALUE(err_node))
    return NULL;

  err_obj = json_node_get_object(err_node);
  if (!json_object_has_member(err_obj, "message"))
    return NULL;

  return json_object_get_string_member(err_obj, "message");
}

gboolean json_rpc_is_request(JsonNode *msg) {
  return json_rpc_get_method(msg) != NULL && json_rpc_get_id(msg) >= 0;
}

gboolean json_rpc_is_response(JsonNode *msg) {
  if (msg == NULL || JSON_NODE_HOLDS_VALUE(msg))
    return FALSE;

  {
    JsonObject *obj = json_node_get_object(msg);

    if (json_rpc_get_id(msg) < 0)
      return FALSE;
    return json_object_has_member(obj, "result") ||
           json_object_has_member(obj, "error");
  }
}

gboolean json_rpc_is_notification(JsonNode *msg) {
  return json_rpc_get_method(msg) != NULL && json_rpc_get_id(msg) < 0;
}

gboolean json_rpc_is_error(JsonNode *msg) {
  return json_rpc_get_error_code(msg) != 0;
}
