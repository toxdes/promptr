#include "provider-manager.h"
#include "config.h"
#include "configfile.h"
#include "plugin-manager.h"
#include "plugin-runner.h"
#include "provider-profile.h"
#include "provider.h"
#include <glib.h>
#include <string.h>

Provider *provider_manager_create_named(RuntimeConfig *config, const char *name,
                                        GError **error) {
  (void)config;
  g_autoptr(ProviderProfile) profile = provider_profile_load(name, error);
  if (profile == NULL)
    return NULL;
  if (!profile->tested && !profile->bundled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Test this provider in Preferences before submitting");
    return NULL;
  }
  return plugin_runner_create_profile(profile);
}

void provider_manager_destroy(Provider *provider) {
  if (provider == NULL)
    return;

  if (provider->vtable != NULL && provider->vtable->destroy != NULL)
    provider->vtable->destroy(provider->impl);

  g_free(provider);
}
