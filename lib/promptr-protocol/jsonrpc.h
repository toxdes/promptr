#ifndef JSONRPC_H
#define JSONRPC_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

/*
 * jsonrpc.h — JSON-RPC 2.0 reference implementation for provider plugins.
 *
 * Provides line-delimited JSON-RPC 2.0 over stdio streams, plus
 * constructors and accessors for the common message types.
 *
 * ALL returned JsonNode pointers are newly allocated. Caller frees
 * with json_node_free(). Accessors return borrowed pointers.
 */

/* ── I/O ───────────────────────────────────────────────── */

JsonNode *json_rpc_read(FILE *stream, GError **error);
gboolean json_rpc_write(FILE *stream, JsonNode *msg, GError **error);

/* ── Constructors ───────────────────────────────────────── */

JsonNode *json_rpc_make_request(const char *method, JsonNode *params, int id);
JsonNode *json_rpc_make_response(JsonNode *result, int id);
JsonNode *json_rpc_make_error(int code, const char *message, int id);
JsonNode *json_rpc_make_notification(const char *method, JsonNode *params);

JsonNode *json_rpc_make_ok(int id);

/* ── Accessors ─────────────────────────────────────────── */

const char *json_rpc_get_method(JsonNode *msg);
JsonNode *json_rpc_get_params(JsonNode *msg);
int json_rpc_get_id(JsonNode *msg);
JsonNode *json_rpc_get_result(JsonNode *msg);
int json_rpc_get_error_code(JsonNode *msg);
const char *json_rpc_get_error_message(JsonNode *msg);

gboolean json_rpc_is_request(JsonNode *msg);
gboolean json_rpc_is_response(JsonNode *msg);
gboolean json_rpc_is_notification(JsonNode *msg);
gboolean json_rpc_is_error(JsonNode *msg);

#endif /* JSONRPC_H */
