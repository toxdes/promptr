#include "provider-manager.h"
#include "config.h"
#include "configfile.h"
#include "provider.h"
#include "providers/opencode.h"
#include <glib.h>
#include <string.h>

Provider *provider_manager_create(RuntimeConfig *config, GError **error) {
  g_autofree char *provider_name = NULL;
  Provider *p;

  provider_name =
      runtime_config_get_string(config, "provider", PROVIDER_DEFAULT);

  p = g_new0(Provider, 1);

  if (g_strcmp0(provider_name, "opencode") == 0) {
    p->vtable = &opencode_vtable;
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown provider: %s",
                provider_name);
    g_free(p);
    return NULL;
  }

  if (!p->vtable->init(&p->impl, config, error)) {
    g_free(p);
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
