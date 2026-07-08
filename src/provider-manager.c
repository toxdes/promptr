#include "provider-manager.h"
#include "config.h"
#include "configfile.h"
#include "plugin-manager.h"
#include "plugin-runner.h"
#include "provider.h"
#include "providers/opencode.h"
#include "providers/openrouter.h"
#include <glib.h>
#include <string.h>

typedef struct {
  const char *name;
  ProviderVTable *vtable;
} ProviderEntry;

static const ProviderEntry KNOWN_PROVIDERS[] = {
    {"opencode", &opencode_vtable},
    {"openrouter", &openrouter_vtable},
};

Provider *provider_manager_create(RuntimeConfig *config, GError **error) {
  g_autofree char *name =
      runtime_config_get_string(config, "provider", PROVIDER_DEFAULT);

  return provider_manager_create_named(config, name, error);
}

Provider *provider_manager_create_named(RuntimeConfig *config, const char *name,
                                        GError **error) {
  Provider *p;

  if (name == NULL || name[0] == '\0')
    name = PROVIDER_DEFAULT;

  /* Try plugin-manager first — resolves from plugin manifests */
  {
    PluginManifest *manifest = plugin_manager_get_manifest(name);
    const char *cmd;

    if (manifest != NULL) {
      cmd = manifest->command_path != NULL ? manifest->command_path
                                           : manifest->command;
      p = plugin_runner_create(cmd, config, error);
      if (p != NULL)
        return p;
      /* Plugin runner failed — fall through to KNOWN_PROVIDERS */
    }
  }

  /* Fall back to compiled-in providers */
  p = g_new0(Provider, 1);
  p->vtable = NULL;

  for (size_t i = 0; i < G_N_ELEMENTS(KNOWN_PROVIDERS); i++) {
    if (g_strcmp0(name, KNOWN_PROVIDERS[i].name) == 0) {
      p->vtable = KNOWN_PROVIDERS[i].vtable;
      break;
    }
  }

  if (p->vtable == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown provider: %s",
                name);
    g_free(p);
    return NULL;
  }

  if (!p->vtable->init(&p->impl, config, error)) {
    g_free(p);
    return NULL;
  }

  return p;
}

Provider *provider_manager_create_or_fallback(RuntimeConfig *config,
                                              const char *name,
                                              char **out_error_msg) {
  GError *error = NULL;
  Provider *p;

  if (out_error_msg != NULL)
    *out_error_msg = NULL;

  /* Try the requested provider */
  if (name != NULL && name[0] != '\0') {
    p = provider_manager_create_named(config, name, &error);
    if (p != NULL)
      return p;
  }

  /* Failed — format descriptive message with provider name + fallback */
  if (out_error_msg != NULL) {
    const char *reason = error != NULL ? error->message : "unknown error";

    *out_error_msg = g_strdup_printf("Provider \"%s\" is not available: %s\n\n"
                                     "Falling back to \"opencode\".",
                                     name, reason);
  }
  g_clear_error(&error);

  /* Fall back to opencode (always available, no deps) */
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
  static const char **names = NULL;
  GPtrArray *arr;
  gboolean seen_opencode = FALSE, seen_openrouter = FALSE;

  if (names != NULL)
    return names;

  arr = g_ptr_array_new();

  /* Add plugin-manager providers first */
  {
    const char *const *plugin_names = plugin_manager_get_providers();

    if (plugin_names != NULL) {
      for (int i = 0; plugin_names[i] != NULL; i++) {
        g_ptr_array_add(arr, g_strdup(plugin_names[i]));
        if (g_strcmp0(plugin_names[i], "opencode") == 0)
          seen_opencode = TRUE;
        if (g_strcmp0(plugin_names[i], "openrouter") == 0)
          seen_openrouter = TRUE;
      }
    }
  }

  /* Add compiled-in providers not already in plugin list */
  if (!seen_opencode)
    g_ptr_array_add(arr, g_strdup("opencode"));
  if (!seen_openrouter)
    g_ptr_array_add(arr, g_strdup("openrouter"));

  g_ptr_array_add(arr, NULL);
  names = (const char **)g_ptr_array_free(arr, FALSE);
  return names;
}
