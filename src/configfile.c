#include "configfile.h"
#include "config.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROUP       "preferences"
#define CONFIG_DIR  DATA_DIR_SUFFIX
#define CONFIG_FILE "config"

struct _RuntimeConfig {
  GKeyFile *kf;
};

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
    {"mark_bg_color", MARK_BG_COLOR,
     "# Hex color for marked-line gutter indicator"},
    {"kb_focus_prompt", KB_FOCUS_PROMPT,
     "# Keyboard shortcuts (GTK accelerator string format,"
     " e.g. \"<Control>k\")"},
    {"kb_copy_marked", KB_COPY_MARKED, NULL},
    {"kb_close", KB_CLOSE, NULL},
    {"kb_quit", KB_QUIT, NULL},
    {"kb_log", KB_LOG, NULL},
    {"kb_shortcuts", KB_SHORTCUTS, NULL},
    {"kb_submit", KB_SUBMIT, NULL},
    {"kb_cancel", KB_CANCEL, NULL},
    {"opencode_path", OPENCODE_PATH, "# Path to the opencode binary"},
    {"agent_options", DEFAULT_AGENT_OPTIONS,
     "# Agent dropdown options (comma-separated, first=default)"},
    {"model_options", DEFAULT_MODEL_OPTIONS,
     "# Model dropdown options (comma-separated, first=default)"},
    {"marked_lines", DEFAULT_MARKED_LINES_STR,
     "# Default marked lines on output"
     " (0=all, -1=none, or comma-separated 1-based)"},
    {"decorated", G_STRINGIFY(DECORATED_DEFAULT),
     "# Window decorations when layer-shell is disabled (0 or 1)"},
    {"command_expanded", G_STRINGIFY(COMMAND_EXPANDED_DEFAULT),
     "# Command section expanded by default (0 or 1)"},
    {"prompt_font_size", G_STRINGIFY(PROMPT_FONT_SIZE_DEFAULT),
     "# Font size in pt for prompt text (0 = system default)"},
    {"output_font_size", G_STRINGIFY(OUTPUT_FONT_SIZE_DEFAULT),
     "# Font size in pt for output text (0 = system default)"},
    {"gsk_renderer", GSK_RENDERER_DEFAULT,
     "# GSK renderer backend (set before gtk_init,"
     " not switchable at runtime):\n"
     "#   cairo  - software (CPU), no GPU allocations,"
     " ~half baseline memory\n"
     "#   ngl    - OpenGL (GPU), GTK 4.14+ default,"
     " highest memory usage\n"
     "#   gl     - OpenGL (GPU), deprecated in 4.14,"
     " aliases to ngl\n"
     "#   vulkan - Vulkan (GPU), similar memory to GL;"
     " requires Vulkan driver"},
    {"kb_layout", KB_LAYOUT, "# Toggle horizontal/vertical layout"},
    {"kb_popout", KB_POPOUT,
     "# Pop out the output text area into its own window"},
    {"layout", LAYOUT_DEFAULT, "# UI layout (\"horizontal\" or \"vertical\")"},
    {"kb_new_tab", KB_NEW_TAB, "# Create new tab"},
    {"kb_close_tab", KB_CLOSE_TAB, "# Close current tab"},
    {"kb_restore_tab", KB_RESTORE_TAB, "# Restore last closed tab"},
    {"kb_follow_up_toggle", KB_FOLLOW_UP_TOGGLE, "# Toggle follow-up checkbox"},
    {"tab_position", TAB_POSITION_DEFAULT,
     "# Tab bar position (\"top\" or \"bottom\")"},
    {"tab_confirm_before_close", G_STRINGIFY(TAB_CONFIRM_BEFORE_CLOSE_DEFAULT),
     "# Show confirmation dialog before closing active tabs (0 or 1)"},
    {"tab_show_add_button", G_STRINGIFY(TAB_SHOW_ADD_BUTTON_DEFAULT),
     "# Show the + (add new tab) button (0 or 1)"},
    {"kb_menu_bar", KB_MENU_BAR, "# Toggle menu bar visibility"},
    {"menu_bar_visible", G_STRINGIFY(MENU_BAR_VISIBLE_DEFAULT),
     "# Show menu bar at startup (0 or 1)"},
    {"kb_status_bar", KB_STATUS_BAR, "# Toggle status bar visibility"},
    {"status_bar_visible", G_STRINGIFY(STATUS_BAR_VISIBLE_DEFAULT),
     "# Show status bar at startup (0 or 1)"},
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
  GString *content;

  dir = g_get_user_config_dir();
  dirpath = g_build_filename(dir, CONFIG_DIR, NULL);
  g_mkdir_with_parents(dirpath, 0700);

  content = g_string_new("# promptr runtime configuration\n"
                         "# Each key falls back to the compile-time"
                         " default if missing.\n"
                         "\n"
                         "[preferences]\n");

  for (int i = 0; CONFIG_DEFAULTS[i].key != NULL; i++) {
    if (CONFIG_DEFAULTS[i].comment != NULL) {
      if (i > 0)
        g_string_append_c(content, '\n');
      g_string_append(content, CONFIG_DEFAULTS[i].comment);
      g_string_append_c(content, '\n');
    }
    g_string_append(content, CONFIG_DEFAULTS[i].key);
    g_string_append_c(content, '=');
    g_string_append(content, CONFIG_DEFAULTS[i].value);
    g_string_append_c(content, '\n');
  }

  g_file_set_contents(path, content->str, -1, NULL);
  g_string_free(content, TRUE);
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
