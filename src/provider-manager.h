#ifndef PROVIDER_MANAGER_H
#define PROVIDER_MANAGER_H

#include "configfile.h"
#include "provider.h"

Provider *provider_manager_create(RuntimeConfig *config, GError **error);
void provider_manager_destroy(Provider *provider);

#endif
