#ifndef PROVIDER_PROFILE_H
#define PROVIDER_PROFILE_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>

typedef struct {
  char *id, *name, *plugin, *command, *model;
  char **arguments;
  JsonObject *settings;
  char **secret_fields;
  JsonObject *secret_refs;
  char **models;
  gboolean tested;
  gboolean bundled;
} ProviderProfile;

ProviderProfile *provider_profile_new(const char *plugin);
ProviderProfile *provider_profile_load(const char *id, GError **error);
ProviderProfile *provider_profile_copy(const ProviderProfile *profile);
void provider_profile_free(ProviderProfile *profile);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ProviderProfile, provider_profile_free)
GPtrArray *provider_profile_list(void);
/* Blocking Secret Service/disk operations; callers use GTask workers. */
JsonObject *provider_profile_resolve(ProviderProfile *profile,
                                     GCancellable *cancel, GError **error);
gboolean provider_profile_save(ProviderProfile *profile, GCancellable *cancel,
                               GError **error);
gboolean provider_profile_delete(ProviderProfile *profile, GCancellable *cancel,
                                 GError **error);
#endif
