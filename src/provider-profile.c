#include "provider-profile.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "config.h"
#include "plugin-manager.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <libsecret/secret.h>

typedef char SecretPassword;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SecretPassword, secret_password_free)

static const SecretSchema profile_schema = {
    .name = "com.toxdes.promptr.Provider",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
                   {"profile", SECRET_SCHEMA_ATTRIBUTE_STRING},
                   {"field", SECRET_SCHEMA_ATTRIBUTE_STRING},
                   {"reference", SECRET_SCHEMA_ATTRIBUTE_STRING},
                   {NULL, 0}}};

static char *profiles_dir(void) {
  return g_build_filename(g_get_user_config_dir(), DATA_DIR_SUFFIX, "providers",
                          NULL);
}

static char *profile_path(const char *id) {
  g_autofree char *dir = profiles_dir();
  g_autofree char *name = g_strconcat(id, ".json", NULL);
  return g_build_filename(dir, name, NULL);
}

static JsonObject *copy_object(JsonObject *object) {
  if (object == NULL)
    return json_object_new();
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  g_autoptr(JsonNode) copy = json_node_copy(node);
  /* JsonNode copies share objects; serialize to obtain an independent settings
   * snapshot. */
  g_autofree char *text = json_to_string(copy, FALSE);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, text, -1, NULL))
    return json_object_new();
  return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

ProviderProfile *provider_profile_new(const char *plugin) {
  ProviderProfile *p = g_new0(ProviderProfile, 1);
  PluginManifest *manifest = plugin_manager_get_manifest(plugin);
  p->id = g_uuid_string_random();
  p->name = g_strdup(plugin);
  p->plugin = g_strdup(plugin);
  p->command = g_strdup(manifest != NULL && manifest->command_path != NULL
                            ? manifest->command_path
                        : manifest != NULL ? manifest->command
                                           : "");
  p->settings = json_object_new();
  p->secret_refs = json_object_new();
  if (manifest != NULL) {
    p->model = g_strdup(manifest->default_model);
    p->models = g_strdupv(manifest->models);
    JsonObject *schema = manifest->config_schema != NULL
                             ? json_node_get_object(manifest->config_schema)
                             : NULL;
    g_autoptr(GList) fields =
        schema != NULL ? json_object_get_members(schema) : NULL;
    g_autoptr(GPtrArray) secrets = g_ptr_array_new_with_free_func(g_free);
    for (GList *it = fields; it != NULL; it = it->next) {
      JsonObject *field = protocol_object(schema, it->data);
      JsonNode *value = json_object_get_member(field, "default");
      if (protocol_boolean(field, "secret"))
        g_ptr_array_add(secrets, g_strdup(it->data));
      else if (value != NULL)
        json_object_set_member(p->settings, it->data, json_node_copy(value));
    }
    g_ptr_array_add(secrets, NULL);
    p->secret_fields =
        (char **)g_ptr_array_free(g_steal_pointer(&secrets), FALSE);
  }
  return p;
}

static ProviderProfile *bundled_profile(const char *id) {
  const char *const *plugins = plugin_manager_get_providers();
  for (guint i = 0; plugins != NULL && plugins[i] != NULL; i++) {
    PluginManifest *m = plugin_manager_get_manifest(plugins[i]);
    if (g_strcmp0(m->profile_id, id) != 0)
      continue;
    ProviderProfile *p = provider_profile_new(m->name);
    g_free(p->id);
    p->id = g_strdup(id);
    g_free(p->name);
    p->name = g_strdup(m->profile_name != NULL ? m->profile_name : m->name);
    /* Shipped defaults are usable immediately; the plugin validates credentials
     * when submitting. User edits still require a successful connection test.
     */
    p->bundled = TRUE;
    return p;
  }
  return NULL;
}

ProviderProfile *provider_profile_copy(const ProviderProfile *p) {
  ProviderProfile *out = g_new0(ProviderProfile, 1);
  out->id = g_strdup(p->id);
  out->name = g_strdup(p->name);
  out->plugin = g_strdup(p->plugin);
  out->command = g_strdup(p->command);
  out->model = g_strdup(p->model);
  out->arguments = g_strdupv(p->arguments);
  out->secret_fields = g_strdupv(p->secret_fields);
  out->secret_refs = copy_object(p->secret_refs);
  out->models = g_strdupv(p->models);
  out->settings = copy_object(p->settings);
  out->tested = p->tested;
  out->bundled = p->bundled;
  return out;
}

void provider_profile_free(ProviderProfile *p) {
  if (p == NULL)
    return;
  g_free(p->id);
  g_free(p->name);
  g_free(p->plugin);
  g_free(p->command);
  g_free(p->model);
  g_strfreev(p->arguments);
  g_strfreev(p->secret_fields);
  if (p->secret_refs != NULL)
    json_object_unref(p->secret_refs);
  g_strfreev(p->models);
  if (p->settings != NULL)
    json_object_unref(p->settings);
  g_free(p);
}

static char **read_strings(JsonObject *o, const char *field) {
  JsonNode *node = json_object_get_member(o, field);
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
    return NULL;
  JsonArray *array = json_node_get_array(node);
  GPtrArray *strings = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *value = json_array_get_element(array, i);
    if (JSON_NODE_HOLDS_VALUE(value) &&
        json_node_get_value_type(value) == G_TYPE_STRING)
      g_ptr_array_add(strings, g_strdup(json_node_get_string(value)));
  }
  g_ptr_array_add(strings, NULL);
  return (char **)g_ptr_array_free(strings, FALSE);
}

ProviderProfile *provider_profile_load(const char *id, GError **error) {
  if (id == NULL || !g_uuid_string_is_valid(id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Choose a provider in Preferences");
    return NULL;
  }
  g_autofree char *path = profile_path(id);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    ProviderProfile *preset = bundled_profile(id);
    if (preset != NULL)
      return preset;
  }
  if (!json_parser_load_from_file(parser, path, error))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *o =
      JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  const char *plugin = protocol_string(o, "plugin");
  if (plugin == NULL || protocol_string(o, "name") == NULL ||
      protocol_string(o, "command") == NULL ||
      g_strcmp0(protocol_string(o, "id"), id) != 0 ||
      protocol_object(o, "settings") == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid provider profile");
    return NULL;
  }
  ProviderProfile *p = provider_profile_new(plugin);
  g_free(p->id);
  p->id = g_strdup(id);
  g_free(p->name);
  p->name = g_strdup(protocol_string(o, "name"));
  g_free(p->command);
  p->command = g_strdup(protocol_string(o, "command"));
  g_free(p->model);
  p->model = g_strdup(protocol_string(o, "model"));
  json_object_unref(p->settings);
  p->settings = json_object_ref(protocol_object(o, "settings"));
  p->arguments = read_strings(o, "arguments");
  g_strfreev(p->secret_fields);
  p->secret_fields = read_strings(o, "secret_fields");
  json_object_unref(p->secret_refs);
  p->secret_refs = copy_object(protocol_object(o, "secret_refs"));
  g_strfreev(p->models);
  p->models = read_strings(o, "models");
  p->tested = protocol_boolean(o, "tested");
  return p;
}

static gint profile_compare(gconstpointer a, gconstpointer b) {
  const ProviderProfile *one = *(ProviderProfile *const *)a;
  const ProviderProfile *two = *(ProviderProfile *const *)b;
  gint result = g_utf8_collate(one->name, two->name);
  return result != 0 ? result : g_strcmp0(one->id, two->id);
}

GPtrArray *provider_profile_list(void) {
  GPtrArray *out =
      g_ptr_array_new_with_free_func((GDestroyNotify)provider_profile_free);
  g_autofree char *dir = profiles_dir();
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  const char *name;
  while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
    if (!g_str_has_suffix(name, ".json"))
      continue;
    g_autofree char *id = g_strndup(name, strlen(name) - 5);
    ProviderProfile *p = provider_profile_load(id, NULL);
    if (p != NULL)
      g_ptr_array_add(out, p);
  }
  const char *const *plugins = plugin_manager_get_providers();
  for (guint i = 0; plugins != NULL && plugins[i] != NULL; i++) {
    PluginManifest *m = plugin_manager_get_manifest(plugins[i]);
    if (m->profile_id == NULL)
      continue;
    gboolean found = FALSE;
    for (guint j = 0; j < out->len; j++)
      if (g_strcmp0(((ProviderProfile *)g_ptr_array_index(out, j))->id,
                    m->profile_id) == 0)
        found = TRUE;
    g_autofree char *path = profile_path(m->profile_id);
    if (!found && !g_file_test(path, G_FILE_TEST_EXISTS)) {
      ProviderProfile *p = bundled_profile(m->profile_id);
      if (p != NULL)
        g_ptr_array_add(out, p);
    }
  }
  g_ptr_array_sort(out, profile_compare);
  return out;
}

JsonObject *provider_profile_resolve(ProviderProfile *p, GCancellable *cancel,
                                     GError **error) {
  JsonObject *settings = copy_object(p->settings);
  g_autoptr(GList) fields = json_object_get_members(p->secret_refs);
  for (GList *it = fields; it != NULL; it = it->next) {
    const char *field = it->data,
               *reference = protocol_string(p->secret_refs, field);
    const char *entered = protocol_string(settings, field);
    if ((entered != NULL && *entered) || reference == NULL)
      continue;
    g_autoptr(SecretPassword) secret = secret_password_lookup_sync(
        &profile_schema, cancel, error, "application", DATA_DIR_SUFFIX,
        "profile", p->id, "field", field, "reference", reference, NULL);
    if (error != NULL && *error != NULL) {
      json_object_unref(settings);
      return NULL;
    }
    if (secret == NULL) {
      json_object_unref(settings);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "Saved credential '%s' is missing; enter it again in Manage "
                  "providers",
                  field);
      return NULL;
    }
    json_object_set_string_member(settings, field, secret);
  }
  return settings;
}

static void write_strings(JsonObject *o, const char *field, char **values) {
  JsonArray *array = json_array_new();
  for (guint i = 0; values != NULL && values[i] != NULL; i++)
    json_array_add_string_element(array, values[i]);
  json_object_set_array_member(o, field, array);
}

gboolean provider_profile_save(ProviderProfile *p, GCancellable *cancel,
                               GError **error) {
  if (!p->tested) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Test the current settings before saving");
    return FALSE;
  }
  g_autoptr(JsonObject) settings = copy_object(p->settings);
  g_autoptr(JsonObject) refs = copy_object(p->secret_refs);
  gboolean ok = TRUE;
  for (guint i = 0; p->secret_fields != NULL && p->secret_fields[i] != NULL;
       i++) {
    const char *field = p->secret_fields[i];
    const char *secret = protocol_string(settings, field);
    if (secret != NULL && *secret) {
      g_autofree char *reference = g_uuid_string_random();
      if (!secret_password_store_sync(
              &profile_schema, SECRET_COLLECTION_DEFAULT, p->name, secret,
              cancel, error, "application", DATA_DIR_SUFFIX, "profile", p->id,
              "field", field, "reference", reference, NULL)) {
        ok = FALSE;
        break;
      }
      json_object_set_string_member(refs, field, reference);
    }
    json_object_remove_member(settings, field);
  }
  if (ok) {
    g_autoptr(JsonObject) o = json_object_new();
    json_object_set_int_member(o, "version", 1);
    json_object_set_string_member(o, "id", p->id);
    json_object_set_string_member(o, "name", p->name);
    json_object_set_string_member(o, "plugin", p->plugin);
    json_object_set_string_member(o, "command", p->command);
    json_object_set_string_member(o, "model", p->model != NULL ? p->model : "");
    json_object_set_boolean_member(o, "tested", TRUE);
    json_object_set_object_member(o, "settings", json_object_ref(settings));
    json_object_set_object_member(o, "secret_refs", json_object_ref(refs));
    write_strings(o, "arguments", p->arguments);
    write_strings(o, "secret_fields", p->secret_fields);
    write_strings(o, "models", p->models);
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, o);
    g_autofree char *data = json_to_string(node, TRUE);
    g_autofree char *dir = profiles_dir();
    g_autofree char *path = profile_path(p->id);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                  "Create providers directory: %s", g_strerror(errno));
      ok = FALSE;
    } else
      ok = g_file_set_contents_full(path, data, -1,
                                    G_FILE_SET_CONTENTS_CONSISTENT |
                                        G_FILE_SET_CONTENTS_DURABLE,
                                    0600, error);
  }
  if (!ok) {
    /* Roll back only newly created references. Existing credentials remain
     * usable by the old atomic profile and any request that already snapshotted
     * it. */
    g_autoptr(GList) fields = json_object_get_members(refs);
    for (GList *it = fields; it != NULL; it = it->next) {
      const char *reference = protocol_string(refs, it->data);
      if (g_strcmp0(reference, protocol_string(p->secret_refs, it->data)) != 0)
        secret_password_clear_sync(&profile_schema, cancel, NULL, "application",
                                   DATA_DIR_SUFFIX, "profile", p->id, "field",
                                   (const char *)it->data, "reference",
                                   reference, NULL);
    }
  }
  return ok;
}

gboolean provider_profile_delete(ProviderProfile *p, GCancellable *cancel,
                                 GError **error) {
  if (json_object_get_size(p->secret_refs) > 0) {
    secret_password_clear_sync(&profile_schema, cancel, error, "application",
                               DATA_DIR_SUFFIX, "profile", p->id, NULL);
    if (error != NULL && *error != NULL)
      return FALSE;
  }
  g_autofree char *path = profile_path(p->id);
  if (g_unlink(path) != 0 && errno != ENOENT) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Delete provider profile: %s", g_strerror(errno));
    return FALSE;
  }
  return TRUE;
}
