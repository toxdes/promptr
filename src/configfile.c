#include "configfile.h"
#include "config.h"

#define GROUP       "preferences"
#define CONFIG_DIR  "promptr"
#define CONFIG_FILE "config"

struct _RuntimeConfig {
  GKeyFile *kf;
};

static const char *DEFAULT_CONFIG =
    "# promptr runtime configuration\n"
    "# Each key falls back to the "
    "compile-time "
    "default if missing.\n"
    "\n"
    "[" GROUP "]\n"
    "# Window size in pixels\n"
    "width=" G_STRINGIFY(
        DEFAULT_WIDTH) "\n"
                       "heig"
                       "ht"
                       "=" G_STRINGIFY(
                           DEFAULT_HEIGHT) "\n"
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
                                           "=" G_STRINGIFY(
                                               LAYER_SHELL_ENABLED) "\n"
                                                                    "\n"
                                                                    "# Desktop "
                                                                    "notificati"
                                                                    "on "
                                                                    "on copy "
                                                                    "(0 or 1)\n"
                                                                    "notify_on_"
                                                                    "copy"
                                                                    "=" G_STRINGIFY(
                                                                        NOTIFY_ON_COPY) "\n"
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
                                                                                        "kb_"
                                                                                        "submit"
                                                                                        "=" KB_SUBMIT
                                                                                        "\n"
                                                                                        "kb_"
                                                                                        "cancel"
                                                                                        "=" KB_CANCEL
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
                                                                                        "=0\n"
                                                                                        "\n"
                                                                                        "# Window decorations when "
                                                                                        "layer-shell is "
                                                                                        "disabled "
                                                                                        "(0 or 1)\n"
                                                                                        "decorated=" G_STRINGIFY(
                                                                                            DECORATED_DEFAULT) "\n"
                                                                                                               "\n"
                                                                                                               "# GSK renderer "
                                                                                                               "(cairo=CPU/"
                                                                                                               "half-memory, "
                                                                                                               "ngl/gl/vulkan"
                                                                                                               "=GPU)\n"
                                                                                                               "gsk_renderer=" GSK_RENDERER_DEFAULT
                                                                                                               "\n";

typedef struct {
  const char *key;
  const char *value;
  const char *comment;
} ConfigDefault;

static const ConfigDefault CONFIG_DEFAULTS[] = {
    {"width", G_STRINGIFY(DEFAULT_WIDTH), "# Window size in pixels"},
    {"height", G_STRINGIFY(DEFAULT_HEIGHT), NULL},
    {"layer_shell", G_STRINGIFY(LAYER_SHELL_ENABLED),
     "# Layer-shell overlay on wlroots compositors (0 or 1)"},
    {"notify_on_copy", G_STRINGIFY(NOTIFY_ON_COPY),
     "# Desktop notification on copy (0 or 1)"},
    {"mark_bg_color", MARK_BG_COLOR, "# Gutter mark color in hex"},
    {"kb_focus_prompt", KB_FOCUS_PROMPT,
     "# Keyboard shortcuts (GTK accelerator format)"},
    {"kb_copy_marked", KB_COPY_MARKED, NULL},
    {"kb_close", KB_CLOSE, NULL},
    {"kb_quit", KB_QUIT, NULL},
    {"kb_log", KB_LOG, NULL},
    {"kb_shortcuts", KB_SHORTCUTS, NULL},
    {"kb_submit", KB_SUBMIT, NULL},
    {"kb_cancel", KB_CANCEL, NULL},
    {"opencode_path", OPENCODE_PATH, "# Path to the opencode binary"},
    {"agent_options", DEFAULT_AGENT_OPTIONS,
     "# Agent dropdown options (comma-separated)"},
    {"model_options", DEFAULT_MODEL_OPTIONS,
     "# Model dropdown options (comma-separated)"},
    {"marked_lines", DEFAULT_MARKED_LINES_STR,
     "# Default marked lines on output (1-based, comma-separated, 0=all)"},
    {"decorated", G_STRINGIFY(DECORATED_DEFAULT),
     "# Window decorations when layer-shell is disabled (0 or 1)"},
    {"command_expanded", G_STRINGIFY(COMMAND_EXPANDED_DEFAULT),
     "# Command section expanded by default (0 or 1)"},
    {"prompt_font_size", G_STRINGIFY(PROMPT_FONT_SIZE_DEFAULT),
     "# Font size in pt for prompt text (0 = system default)"},
    {"output_font_size", G_STRINGIFY(OUTPUT_FONT_SIZE_DEFAULT),
     "# Font size in pt for output text (0 = system default)"},
    {"gsk_renderer", GSK_RENDERER_DEFAULT,
     "# GSK renderer (cairo=CPU/half-memory, ngl/gl/vulkan=GPU)"},
    {NULL, NULL, NULL}};

static void migrate_config(const char *path, GKeyFile *kf) {
  g_autofree char *contents = NULL;
  gsize len;
  GString *out;
  gboolean changed = FALSE;
  gboolean cleaned = FALSE;
  g_autoptr(GError) error = NULL;
  g_autofree gchar **keys = NULL;
  gsize n_keys;

  keys = g_key_file_get_keys(kf, GROUP, &n_keys, NULL);

  for (int i = 0; CONFIG_DEFAULTS[i].key != NULL; i++) {
    if (!g_key_file_has_key(kf, GROUP, CONFIG_DEFAULTS[i].key, NULL)) {
      changed = TRUE;
      break;
    }
  }

  for (gsize i = 0; keys != NULL && i < n_keys; i++) {
    gboolean known = FALSE;
    for (int j = 0; CONFIG_DEFAULTS[j].key != NULL; j++)
      if (g_strcmp0(keys[i], CONFIG_DEFAULTS[j].key) == 0) {
        known = TRUE;
        break;
      }
    if (!known) {
      g_key_file_remove_key(kf, GROUP, keys[i], NULL);
      cleaned = TRUE;
    }
  }

  if (!changed && !cleaned)
    return;

  if (!g_file_get_contents(path, &contents, &len, &error)) {
    g_warning("migrate_config: cannot read %s: %s", path, error->message);
    return;
  }

  out = g_string_new(contents);

  for (int i = 0; CONFIG_DEFAULTS[i].key != NULL; i++) {
    if (!g_key_file_has_key(kf, GROUP, CONFIG_DEFAULTS[i].key, NULL)) {
      if (CONFIG_DEFAULTS[i].comment != NULL) {
        g_string_append_c(out, '\n');
        g_string_append(out, CONFIG_DEFAULTS[i].comment);
        g_string_append_c(out, '\n');
      }
      g_string_append(out, CONFIG_DEFAULTS[i].key);
      g_string_append_c(out, '=');
      g_string_append(out, CONFIG_DEFAULTS[i].value);
      g_string_append_c(out, '\n');
      g_key_file_set_value(kf, GROUP, CONFIG_DEFAULTS[i].key,
                           CONFIG_DEFAULTS[i].value);
    }
  }

  if (cleaned) {
    g_autofree char **lines;
    GString *clean;
    gboolean skip;

    lines = g_strsplit(out->str, "\n", -1);
    clean = g_string_new(NULL);

    for (int i = 0; lines[i] != NULL; i++) {
      g_autofree char *stripped = g_strstrip(g_strdup(lines[i]));
      skip = FALSE;

      for (gsize k = 0; keys != NULL && k < n_keys && !skip; k++) {
        g_autofree char *prefix = g_strdup_printf("%s=", keys[k]);

        if (g_str_has_prefix(stripped, prefix)) {
          gboolean known = FALSE;

          for (int j = 0; CONFIG_DEFAULTS[j].key != NULL; j++)
            if (g_strcmp0(keys[k], CONFIG_DEFAULTS[j].key) == 0) {
              known = TRUE;
              break;
            }
          if (!known)
            skip = TRUE;
        }
      }

      if (!skip)
        g_string_append_printf(clean, "%s\n", lines[i]);
    }

    g_string_free(out, TRUE);
    out = clean;
  }

  g_file_set_contents(path, out->str, -1, NULL);
  g_string_free(out, TRUE);
}

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

  migrate_config(path, c->kf);

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
