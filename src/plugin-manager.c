#include "plugin-manager.h"
#include "configfile.h"
#include <errno.h>
#include <string.h>

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
  g_free(m->description);
  g_free(m->homepage);
  g_free(m->type);
  g_strfreev(m->methods);
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
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
    return NULL;

  obj = json_node_get_object(root);

  if (!json_object_has_member(obj, "name"))
    return NULL;
  if (!json_object_has_member(obj, "command"))
    return NULL;

  m = g_new0(PluginManifest, 1);
  m->name = g_strdup(json_object_get_string_member(obj, "name"));
  m->command = g_strdup(json_object_get_string_member(obj, "command"));

  if (json_object_has_member(obj, "version"))
    m->version = g_strdup(json_object_get_string_member(obj, "version"));

  if (json_object_has_member(obj, "description"))
    m->description =
        g_strdup(json_object_get_string_member(obj, "description"));

  if (json_object_has_member(obj, "homepage"))
    m->homepage = g_strdup(json_object_get_string_member(obj, "homepage"));

  if (json_object_has_member(obj, "type"))
    m->type = g_strdup(json_object_get_string_member(obj, "type"));
  else
    m->type = g_strdup("provider");

  if (json_object_has_member(obj, "methods")) {
    JsonArray *arr = json_object_get_array_member(obj, "methods");

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

    if (schema != NULL)
      m->config_schema = json_node_copy(schema);
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
    g_autofree char *sys_dir =
        g_build_filename("/usr", "lib", DATA_DIR_SUFFIX, "plugins", NULL);

    scan_dir(sys_dir);
  }
}

const char *const *plugin_manager_get_providers(void) {
  if (state.manifests == NULL)
    plugin_manager_scan();

  if (state.names->len == 0)
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
