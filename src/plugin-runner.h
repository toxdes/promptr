#ifndef PLUGIN_RUNNER_H
#define PLUGIN_RUNNER_H

#include "configfile.h"
#include "provider-profile.h"
#include "provider.h"

Provider *plugin_runner_create_profile(const ProviderProfile *profile);

#endif
