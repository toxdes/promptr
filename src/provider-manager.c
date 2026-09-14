#include "provider-manager.h"
#include "config.h"
#include "configfile.h"
#include "plugin-manager.h"
#include "plugin-runner.h"
#include "provider.h"
#include <glib.h>
#include <string.h>

Provider *provider_manager_create(RuntimeConfig *config, GError **error) {
  g_autofree char *name =
      runtime_config_get_string(config, "provider", PROVIDER_DEFAULT);

  return provider_manager_create_named(config, name, error);
}

Provider *provider_manager_create_named(RuntimeConfig *config, const char *name,
                                        GError **error) {
  PluginManifest *manifest;

  if (name == NULL || name[0] == '\0')
    name = PROVIDER_DEFAULT;

  manifest = plugin_manager_get_manifest(name);
  if (manifest == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown provider: %s",
                name);
    return NULL;
  }

  {
    const char *cmd = manifest->command_path != NULL ? manifest->command_path
                                                     : manifest->command;

    return plugin_runner_create(cmd, config, error);
  }
}

Provider *provider_manager_create_or_fallback(RuntimeConfig *config,
                                              const char *name,
                                              char **out_error_msg) {
  GError *error = NULL;
  Provider *p;

  if (out_error_msg != NULL)
    *out_error_msg = NULL;

  if (name != NULL && name[0] != '\0') {
    p = provider_manager_create_named(config, name, &error);
    if (p != NULL)
      return p;
  }

  if (out_error_msg != NULL) {
    const char *reason = error != NULL ? error->message : "unknown error";

    *out_error_msg = g_strdup_printf("Provider \"%s\" is not available: %s\n\n"
                                     "Falling back to \"opencode\".",
                                     name, reason);
  }
  g_clear_error(&error);

  /* Fall back to opencode */
  p = provider_manager_create_named(config, "opencode", &error);
  if (p == NULL) {
    if (out_error_msg != NULL)
      *out_error_msg = g_strdup(error != NULL ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  return p;
}

void provider_manager_destroy(Provider *provider) {
  if (provider == NULL)
    return;

  if (provider->vtable != NULL && provider->vtable->destroy != NULL)
    provider->vtable->destroy(provider->impl);

  g_free(provider);
}

const char *const *provider_manager_get_names(void) {
  return plugin_manager_get_providers();
}
