#ifndef PLUGIN_RUNNER_H
#define PLUGIN_RUNNER_H

#include "configfile.h"
#include "provider.h"

Provider *plugin_runner_create(const char *plugin_command,
                               RuntimeConfig *config, GError **error);

#endif
