#ifndef PROVIDER_MANAGER_H
#define PROVIDER_MANAGER_H

#include "configfile.h"
#include "provider.h"

Provider *provider_manager_create(RuntimeConfig *config, GError **error);
Provider *provider_manager_create_named(RuntimeConfig *config, const char *name,
                                        GError **error);
void provider_manager_destroy(Provider *provider);
Provider *provider_manager_create_or_fallback(RuntimeConfig *config,
                                              const char *name,
                                              char **out_error_msg);

const char *const *provider_manager_get_names(void);

#endif
