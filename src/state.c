#include "state.h"

#define STATE_DIR  "promptr"
#define STATE_FILE "state"

#define GROUP      "state"
#define KEY_MODEL  "model"
#define KEY_AGENT  "agent"

gboolean state_save(const char *model, const char *agent) {
  const char *datadir;
  g_autofree char *dirpath = NULL;
  g_autofree char *filepath = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *data = NULL;
  gsize len;
  g_autoptr(GError) error = NULL;

  datadir = g_get_user_data_dir();
  dirpath = g_build_filename(datadir, STATE_DIR, NULL);
  filepath = g_build_filename(datadir, STATE_DIR, STATE_FILE, NULL);

  if (g_mkdir_with_parents(dirpath, 0700) != 0) {
    g_warning("Failed to create state directory %s", dirpath);
    return FALSE;
  }

  keyfile = g_key_file_new();

  if (model != NULL && model[0] != '\0')
    g_key_file_set_string(keyfile, GROUP, KEY_MODEL, model);
  if (agent != NULL && agent[0] != '\0')
    g_key_file_set_string(keyfile, GROUP, KEY_AGENT, agent);

  data = g_key_file_to_data(keyfile, &len, NULL);

  if (!g_file_set_contents(filepath, data, (gssize)len, &error)) {
    g_warning("Failed to write state file %s: %s", filepath, error->message);
    return FALSE;
  }

  return TRUE;
}

gboolean state_load(char **model_out, char **agent_out) {
  const char *datadir;
  g_autofree char *filepath = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;

  datadir = g_get_user_data_dir();
  filepath = g_build_filename(datadir, STATE_DIR, STATE_FILE, NULL);

  keyfile = g_key_file_new();

  if (!g_key_file_load_from_file(keyfile, filepath, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_warning("Failed to load state file %s: %s", filepath, error->message);
    g_clear_error(&error);
    return FALSE;
  }

  *model_out = g_key_file_get_string(keyfile, GROUP, KEY_MODEL, NULL);
  *agent_out = g_key_file_get_string(keyfile, GROUP, KEY_AGENT, NULL);

  return TRUE;
}
