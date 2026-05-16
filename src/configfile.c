#include "configfile.h"
#include "config.h"

#define GROUP       "preferences"
#define CONFIG_DIR  "promptr"
#define CONFIG_FILE "config"

struct _RuntimeConfig {
  GKeyFile *kf;
};

static const char *DEFAULT_CONFIG = "# promptr runtime configuration\n"
                                    "# Each key falls back to the "
                                    "compile-time "
                                    "default if missing.\n"
                                    "\n"
                                    "[" GROUP "]\n"
                                    "# Window size in pixels\n"
                                    "width=" G_STRINGIFY(DEFAULT_WIDTH) "\n"
                                                                        "heig"
                                                                        "ht"
                                                                        "=" G_STRINGIFY(
                                                                            DEFAULT_HEIGHT) "\n"
                                                                                            "\n"
                                                                                            "# Escape key hides the window (0 "
                                                                                            "or 1)\n"
                                                                                            "escape_hides=" G_STRINGIFY(
                                                                                                ESCAPE_HIDES_WINDOW) "\n"
                                                                                                                     "\n"
                                                                                                                     "# "
                                                                                                                     "Layer-"
                                                                                                                     "shell "
                                                                                                                     "overlay "
                                                                                                                     "on "
                                                                                                                     "wlroots "
                                                                                                                     "compositor"
                                                                                                                     "s (0 or "
                                                                                                                     "1)\n"
                                                                                                                     "layer_"
                                                                                                                     "shell"
                                                                                                                     "=" G_STRINGIFY(LAYER_SHELL_ENABLED) "\n"
                                                                                                                                                          "\n"
                                                                                                                                                          "# Desktop notification "
                                                                                                                                                          "on copy (0 or 1)\n"
                                                                                                                                                          "notify_on_"
                                                                                                                                                          "copy=" G_STRINGIFY(NOTIFY_ON_COPY) "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Gutte"
                                                                                                                                                                                              "r "
                                                                                                                                                                                              "mark "
                                                                                                                                                                                              "color"
                                                                                                                                                                                              " in "
                                                                                                                                                                                              "hex\n"
                                                                                                                                                                                              "mark_"
                                                                                                                                                                                              "bg_"
                                                                                                                                                                                              "color"
                                                                                                                                                                                              "=" MARK_BG_COLOR
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Keybo"
                                                                                                                                                                                              "ard "
                                                                                                                                                                                              "short"
                                                                                                                                                                                              "cuts "
                                                                                                                                                                                              "(GTK "
                                                                                                                                                                                              "accel"
                                                                                                                                                                                              "erato"
                                                                                                                                                                                              "r "
                                                                                                                                                                                              "forma"
                                                                                                                                                                                              "t)\n"
                                                                                                                                                                                              "kb_"
                                                                                                                                                                                              "focus"
                                                                                                                                                                                              "_prom"
                                                                                                                                                                                              "pt"
                                                                                                                                                                                              "=" KB_FOCUS_PROMPT
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "kb_"
                                                                                                                                                                                              "copy_"
                                                                                                                                                                                              "marke"
                                                                                                                                                                                              "d"
                                                                                                                                                                                              "=" KB_COPY_MARKED
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "kb_"
                                                                                                                                                                                              "quit"
                                                                                                                                                                                              "=" KB_QUIT
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Path "
                                                                                                                                                                                              "to "
                                                                                                                                                                                              "the "
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode "
                                                                                                                                                                                              "binar"
                                                                                                                                                                                              "y\n"
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode_"
                                                                                                                                                                                              "path"
                                                                                                                                                                                              "=" OPENCODE_PATH
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Agent"
                                                                                                                                                                                              " drop"
                                                                                                                                                                                              "down "
                                                                                                                                                                                              "optio"
                                                                                                                                                                                              "ns "
                                                                                                                                                                                              "(comm"
                                                                                                                                                                                              "a-"
                                                                                                                                                                                              "separ"
                                                                                                                                                                                              "ated)"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "agent"
                                                                                                                                                                                              "_opti"
                                                                                                                                                                                              "ons="
                                                                                                                                                                                              "linux"
                                                                                                                                                                                              "_cmd,"
                                                                                                                                                                                              "None"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Model"
                                                                                                                                                                                              " drop"
                                                                                                                                                                                              "down "
                                                                                                                                                                                              "optio"
                                                                                                                                                                                              "ns "
                                                                                                                                                                                              "(comm"
                                                                                                                                                                                              "a-"
                                                                                                                                                                                              "separ"
                                                                                                                                                                                              "ated)"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "model"
                                                                                                                                                                                              "_opti"
                                                                                                                                                                                              "ons="
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode-"
                                                                                                                                                                                              "go/"
                                                                                                                                                                                              "deeps"
                                                                                                                                                                                              "eek-"
                                                                                                                                                                                              "v4-"
                                                                                                                                                                                              "flash"
                                                                                                                                                                                              ","
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode-"
                                                                                                                                                                                              "go/"
                                                                                                                                                                                              "deeps"
                                                                                                                                                                                              "eek-"
                                                                                                                                                                                              "v4-"
                                                                                                                                                                                              "pro,"
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode/"
                                                                                                                                                                                              "big-"
                                                                                                                                                                                              "pickl"
                                                                                                                                                                                              "e,"
                                                                                                                                                                                              "openc"
                                                                                                                                                                                              "ode/"
                                                                                                                                                                                              "deeps"
                                                                                                                                                                                              "eek-"
                                                                                                                                                                                              "v4-"
                                                                                                                                                                                              "flash"
                                                                                                                                                                                              "-free"
                                                                                                                                                                                              ",None"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "# "
                                                                                                                                                                                              "Defau"
                                                                                                                                                                                              "lt "
                                                                                                                                                                                              "marke"
                                                                                                                                                                                              "d "
                                                                                                                                                                                              "lines"
                                                                                                                                                                                              " on "
                                                                                                                                                                                              "outpu"
                                                                                                                                                                                              "t "
                                                                                                                                                                                              "(1-"
                                                                                                                                                                                              "based"
                                                                                                                                                                                              ", "
                                                                                                                                                                                              "comma"
                                                                                                                                                                                              "-sepa"
                                                                                                                                                                                              "rated"
                                                                                                                                                                                              ", "
                                                                                                                                                                                              "0="
                                                                                                                                                                                              "all)"
                                                                                                                                                                                              "\n"
                                                                                                                                                                                              "marke"
                                                                                                                                                                                              "d_"
                                                                                                                                                                                              "lines"
                                                                                                                                                                                              "=0\n";

static void write_default_config(const char *path) {
  const char *dir;
  g_autofree char *dirpath;

  dir = g_get_user_config_dir();
  dirpath = g_build_filename(dir, CONFIG_DIR, NULL);
  g_mkdir_with_parents(dirpath, 0700);
  g_file_set_contents(path, DEFAULT_CONFIG, -1, NULL);
}

RuntimeConfig *runtime_config_load(void) {
  RuntimeConfig *c;
  const char *dir;
  g_autofree char *path;
  g_autoptr(GError) error = NULL;

  dir = g_get_user_config_dir();
  path = g_build_filename(dir, CONFIG_DIR, CONFIG_FILE, NULL);

  c = g_new0(RuntimeConfig, 1);
  c->kf = g_key_file_new();

  if (!g_key_file_load_from_file(c->kf, path, G_KEY_FILE_NONE, &error)) {
    if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&error);
      write_default_config(path);
      g_key_file_load_from_file(c->kf, path, G_KEY_FILE_NONE, NULL);
    } else {
      g_warning("Failed to load config %s: %s", path, error->message);
      g_clear_error(&error);
    }
  }

  return c;
}

void runtime_config_free(RuntimeConfig *c) {
  if (c == NULL)
    return;
  g_key_file_free(c->kf);
  g_free(c);
}

int runtime_config_get_int(RuntimeConfig *c, const char *key, int fallback) {
  g_autoptr(GError) error = NULL;
  int v;

  v = g_key_file_get_integer(c->kf, GROUP, key, &error);
  if (error != NULL) {
    g_clear_error(&error);
    return fallback;
  }
  return v;
}

gboolean runtime_config_get_bool(RuntimeConfig *c, const char *key,
                                 gboolean fallback) {
  g_autoptr(GError) error = NULL;
  gboolean v;

  v = g_key_file_get_boolean(c->kf, GROUP, key, &error);
  if (error != NULL) {
    g_clear_error(&error);
    return fallback;
  }
  return v;
}

char *runtime_config_get_string(RuntimeConfig *c, const char *key,
                                const char *fallback) {
  g_autoptr(GError) error = NULL;
  g_autofree char *v;

  v = g_key_file_get_string(c->kf, GROUP, key, &error);
  if (error != NULL) {
    g_clear_error(&error);
    return g_strdup(fallback);
  }
  return g_steal_pointer(&v);
}

char **runtime_config_get_string_list(RuntimeConfig *c, const char *key) {
  g_autoptr(GError) error = NULL;
  g_autofree char *raw;
  char **parts;

  raw = g_key_file_get_string(c->kf, GROUP, key, &error);
  if (error != NULL) {
    g_clear_error(&error);
    return NULL;
  }

  parts = g_strsplit(raw, ",", -1);
  for (int i = 0; parts[i] != NULL; i++)
    parts[i] = g_strstrip(parts[i]);

  return parts;
}
