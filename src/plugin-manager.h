#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct {
  char *name;
  char *version;
  char *command;      /* command name from manifest */
  char *command_path; /* resolved absolute path, or NULL */
  char *description;
  char *homepage;
  char *type;
  char **methods;
  JsonNode *config_schema;
} PluginManifest;

void plugin_manager_scan(void);

const char *const *plugin_manager_get_providers(void);
PluginManifest *plugin_manager_get_manifest(const char *name);

void plugin_manager_shutdown(void);

#endif
