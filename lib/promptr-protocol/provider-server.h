#ifndef PROVIDER_SERVER_H
#define PROVIDER_SERVER_H
#include "jsonrpc.h"
#include <gio/gio.h>

typedef gboolean (*ProviderEmit)(const char *text, gpointer data,
                                 GError **error);
typedef struct {
  gboolean (*initialize)(JsonObject *settings, gboolean discovery,
                         GError **error);
  gboolean (*submit)(JsonObject *settings, JsonObject *request,
                     GCancellable *cancel, ProviderEmit emit,
                     gpointer emit_data, char **resume_state, GError **error);
  char **(*models)(JsonObject *settings, GCancellable *cancel, GError **error);
} ProviderBackend;
int provider_server_run(const ProviderBackend *backend);
char *provider_history_digest(JsonArray *messages, guint count);
void provider_digest_message(GChecksum *checksum, const char *role,
                             const char *text);
#endif
