#include "plugin-manager.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "config.h"
#include "configfile.h"
#include <errno.h>
#include <string.h>

#ifndef PROMPTR_PLUGIN_DIR_DEFAULT
#define PROMPTR_PLUGIN_DIR_DEFAULT "/usr/local/lib/promptr/plugins"
#endif

typedef struct {
  GHashTable *manifests;
  GPtrArray *names;
} PluginManagerState;

static PluginManagerState state = {NULL, NULL};

static void manifest_free(gpointer data) {
  PluginManifest *m = data;

  if (m == NULL)
    return;
  g_free(m->name);
  g_free(m->version);
  g_free(m->command);
  g_free(m->command_path);
  g_free(m->description);
  g_free(m->setup_hint);
  g_free(m->homepage);
  g_free(m->type);
  g_strfreev(m->methods);
  g_free(m->profile_id);
  g_free(m->profile_name);
  g_free(m->default_model);
  g_strfreev(m->models);
  if (m->config_schema != NULL)
    json_node_free(m->config_schema);
  g_free(m);
}

static PluginManifest *manifest_parse(const char *path) {
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonObject *obj;
  PluginManifest *m;

  parser = json_parser_new();
  if (!json_parser_load_from_file(parser, path, &error)) {
    g_warning("plugin-manager: failed to parse %s: %s", path, error->message);
    return NULL;
  }

  root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    return NULL;

  obj = json_node_get_object(root);

  if (protocol_string(obj, "name") == NULL)
    return NULL;
  if (protocol_string(obj, "command") == NULL)
    return NULL;
  JsonNode *manifest_version = json_object_get_member(obj, "manifest_version");
  JsonNode *versions = json_object_get_member(obj, "protocol_versions");
  gboolean compatible = FALSE;
  if (manifest_version == NULL || !JSON_NODE_HOLDS_VALUE(manifest_version) ||
      json_node_get_value_type(manifest_version) != G_TYPE_INT64 ||
      json_node_get_int(manifest_version) != 1 || versions == NULL ||
      !JSON_NODE_HOLDS_ARRAY(versions))
    return NULL;
  JsonArray *supported = json_node_get_array(versions);
  for (guint i = 0; i < json_array_get_length(supported); i++) {
    JsonNode *v = json_array_get_element(supported, i);
    if (JSON_NODE_HOLDS_VALUE(v) &&
        json_node_get_value_type(v) == G_TYPE_STRING &&
        g_strcmp0(json_node_get_string(v), PROMPTR_PROTOCOL_VERSION) == 0)
      compatible = TRUE;
  }
  if (!compatible || (protocol_string(obj, "type") != NULL &&
                      g_strcmp0(protocol_string(obj, "type"), "provider") != 0))
    return NULL;

  m = g_new0(PluginManifest, 1);
  m->name = g_strdup(json_object_get_string_member(obj, "name"));
  m->command = g_strdup(json_object_get_string_member(obj, "command"));
  JsonObject *preset = protocol_object(obj, "bundled_profile");
  const char *preset_id = protocol_string(preset, "id");
  if (preset_id != NULL && g_uuid_string_is_valid(preset_id)) {
    m->profile_id = g_strdup(preset_id);
    m->profile_name = g_strdup(protocol_string(preset, "name"));
    m->default_model = g_strdup(protocol_string(preset, "model"));
  }
  JsonNode *model_list = json_object_get_member(obj, "models");
  if (model_list != NULL && JSON_NODE_HOLDS_ARRAY(model_list)) {
    JsonArray *array = json_node_get_array(model_list);
    g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < json_array_get_length(array) && i < 10000; i++) {
      JsonNode *value = json_array_get_element(array, i);
      if (JSON_NODE_HOLDS_VALUE(value) &&
          json_node_get_value_type(value) == G_TYPE_STRING &&
          strlen(json_node_get_string(value)) <= 1024)
        g_ptr_array_add(values, g_strdup(json_node_get_string(value)));
    }
    g_ptr_array_add(values, NULL);
    m->models = (char **)g_ptr_array_free(g_steal_pointer(&values), FALSE);
  }

  /* Resolve command to absolute path */
  {
    g_autofree char *dir = g_path_get_dirname(path);
    g_autofree char *resolved = g_build_filename(dir, m->command, NULL);

    if (g_file_test(resolved, G_FILE_TEST_IS_EXECUTABLE))
      m->command_path = g_steal_pointer(&resolved);
    else if (g_path_is_absolute(m->command))
      m->command_path = g_strdup(m->command);
  }

  m->version = g_strdup(protocol_string(obj, "version"));
  m->setup_hint = g_strdup(protocol_string(obj, "setup_hint"));

  if (json_object_has_member(obj, "description"))
    m->description = g_strdup(protocol_string(obj, "description"));

  if (json_object_has_member(obj, "homepage"))
    m->homepage = g_strdup(protocol_string(obj, "homepage"));

  if (json_object_has_member(obj, "type"))
    m->type = g_strdup(protocol_string(obj, "type"));
  else
    m->type = g_strdup("provider");

  if (json_object_has_member(obj, "methods")) {
    JsonNode *methods = json_object_get_member(obj, "methods");
    JsonArray *arr =
        JSON_NODE_HOLDS_ARRAY(methods) ? json_node_get_array(methods) : NULL;

    if (arr != NULL) {
      GPtrArray *strs = g_ptr_array_new();

      for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonNode *elem = json_array_get_element(arr, i);

        if (elem != NULL && JSON_NODE_HOLDS_VALUE(elem) &&
            json_node_get_value_type(elem) == G_TYPE_STRING)
          g_ptr_array_add(strs, g_strdup(json_node_get_string(elem)));
      }
      g_ptr_array_add(strs, NULL);
      m->methods = (char **)g_ptr_array_free(strs, FALSE);
    }
  }

  if (json_object_has_member(obj, "config_schema")) {
    JsonNode *schema = json_object_get_member(obj, "config_schema");

    if (schema != NULL && JSON_NODE_HOLDS_OBJECT(schema)) {
      JsonObject *fields = json_node_get_object(schema);
      g_autoptr(GList) keys = json_object_get_members(fields);
      for (GList *it = keys; it != NULL; it = it->next) {
        JsonObject *field = protocol_object(fields, it->data);
        const char *type = protocol_string(field, "type");
        if (g_strcmp0(type, "string") != 0 && g_strcmp0(type, "boolean") != 0 &&
            g_strcmp0(type, "number") != 0 && g_strcmp0(type, "choice") != 0) {
          manifest_free(m);
          return NULL;
        }
        JsonNode *secret = json_object_get_member(field, "secret");
        if (secret != NULL &&
            (!JSON_NODE_HOLDS_VALUE(secret) ||
             json_node_get_value_type(secret) != G_TYPE_BOOLEAN ||
             (json_node_get_boolean(secret) &&
              g_strcmp0(type, "string") != 0))) {
          manifest_free(m);
          return NULL;
        }
      }
      m->config_schema = json_node_copy(schema);
    }
  }

  return m;
}

static void scan_dir(const char *dirpath) {
  GDir *dir;
  const char *entry;

  dir = g_dir_open(dirpath, 0, NULL);
  if (dir == NULL)
    return;

  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *plugin_dir = g_build_filename(dirpath, entry, NULL);
    g_autofree char *manifest_path =
        g_build_filename(plugin_dir, "plugin.json", NULL);
    PluginManifest *m;

    if (!g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR))
      continue;

    m = manifest_parse(manifest_path);
    if (m == NULL)
      continue;

    if (g_hash_table_lookup(state.manifests, m->name) != NULL) {
      manifest_free(m);
      continue;
    }

    g_hash_table_insert(state.manifests, g_strdup(m->name), m);
    g_ptr_array_add(state.names, g_strdup(m->name));
  }

  g_dir_close(dir);
}

void plugin_manager_scan(void) {
  const char *env_dir;

  if (state.manifests != NULL)
    return;

  state.manifests =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, manifest_free);
  state.names = g_ptr_array_new_with_free_func(g_free);

  env_dir = g_getenv("PROMPTR_PLUGIN_DIR");
  if (env_dir != NULL)
    scan_dir(env_dir);

  {
    g_autofree char *user_dir = g_build_filename(
        g_get_user_config_dir(), DATA_DIR_SUFFIX, "plugins", NULL);

    scan_dir(user_dir);
  }

  {
    /* Find bundled adapters after a custom-prefix install or archive
     * relocation, even when the build used a different PREFIX. No
     * current-directory lookup. */
    g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL) {
      g_autofree char *bin = g_path_get_dirname(exe);
      g_autofree char *prefix = g_path_get_dirname(bin);
      g_autofree char *bundled =
          g_build_filename(prefix, "lib", "promptr", "plugins", NULL);
      scan_dir(bundled);
      /* A source checkout keeps bundled plugins next to the debug executable.
       * Resolve from the executable, never from an arbitrary working directory.
       */
      g_autofree char *development = g_build_filename(bin, "providers", NULL);
      scan_dir(development);
    }
    scan_dir(PROMPTR_PLUGIN_DIR_DEFAULT);
    scan_dir("/usr/lib/promptr/plugins");
  }

  g_ptr_array_add(state.names, NULL);
}

const char *const *plugin_manager_get_providers(void) {
  if (state.manifests == NULL)
    plugin_manager_scan();

  if (state.names == NULL || state.names->len <= 1)
    return NULL;

  return (const char *const *)state.names->pdata;
}

PluginManifest *plugin_manager_get_manifest(const char *name) {
  if (state.manifests == NULL)
    plugin_manager_scan();

  return g_hash_table_lookup(state.manifests, name);
}

void plugin_manager_shutdown(void) {
  if (state.manifests != NULL) {
    g_hash_table_destroy(state.manifests);
    state.manifests = NULL;
  }
  if (state.names != NULL) {
    g_ptr_array_unref(state.names);
    state.names = NULL;
  }
}
