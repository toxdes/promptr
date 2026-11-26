#include "shortcuts.h"
#include "config.h"
#include <string.h>

static const ShortcutDefinition definitions[] = {
    {"Editor", "Focus prompt", "kb_focus_prompt", KB_FOCUS_PROMPT},
    {"Editor", "Submit", "kb_submit", KB_SUBMIT},
    {"Editor", "Copy marked lines", "kb_copy_marked", KB_COPY_MARKED},
    {"Editor", "Cancel request", "kb_cancel", KB_CANCEL},
    {"Conversation", "New tab", "kb_new_tab", KB_NEW_TAB},
    {"Conversation", "Close tab", "kb_close_tab", KB_CLOSE_TAB},
    {"Conversation", "Restore tab", "kb_restore_tab", KB_RESTORE_TAB},
    {"Conversation", "Toggle follow-up", "kb_follow_up_toggle",
     KB_FOLLOW_UP_TOGGLE},
    {"View", "Toggle layout", "kb_layout", KB_LAYOUT},
    {"View", "Pop out output", "kb_popout", KB_POPOUT},
    {"View", "Toggle menu bar", "kb_menu_bar", KB_MENU_BAR},
    {"View", "Toggle status bar", "kb_status_bar", KB_STATUS_BAR},
    {"Selection", "Next agent", "kb_cycle_agent_next", KB_CYCLE_AGENT_NEXT},
    {"Selection", "Previous agent", "kb_cycle_agent_prev", KB_CYCLE_AGENT_PREV},
    {"Selection", "Next model", "kb_cycle_model_next", KB_CYCLE_MODEL_NEXT},
    {"Selection", "Previous model", "kb_cycle_model_prev", KB_CYCLE_MODEL_PREV},
    {"Window", "Session log", "kb_log", KB_LOG},
    {"Window", "Keyboard shortcuts", "kb_shortcuts", KB_SHORTCUTS},
    {"Window", "Close window", "kb_close", KB_CLOSE},
    {"Window", "Quit", "kb_quit", KB_QUIT},
};

const ShortcutDefinition *shortcut_definitions(gsize *count) {
  *count = G_N_ELEMENTS(definitions);
  return definitions;
}

char *shortcut_value(RuntimeConfig *config, const char *key) {
  for (guint i = 0; i < G_N_ELEMENTS(definitions); i++)
    if (g_str_equal(key, definitions[i].key))
      return runtime_config_get_string(config, key, definitions[i].fallback);
  g_return_val_if_reached(NULL);
}

char *shortcut_human(RuntimeConfig *config, const char *key) {
  g_autofree char *value = shortcut_value(config, key);
  return accel_to_human(value);
}

char *accel_to_human(const char *accel) {
  GString *out;
  const char *p;

  out = g_string_new(NULL);
  p = accel != NULL ? accel : "";

  while (*p != '\0') {
    if (*p == '<') {
      p++;
      if (g_ascii_strncasecmp(p, "Control>", 8) == 0) {
        g_string_append(out, "ctrl+");
        p += 8;
      } else if (g_ascii_strncasecmp(p, "Shift>", 6) == 0) {
        g_string_append(out, "shift+");
        p += 6;
      } else if (g_ascii_strncasecmp(p, "Alt>", 4) == 0) {
        g_string_append(out, "alt+");
        p += 4;
      } else if (g_ascii_strncasecmp(p, "Super>", 6) == 0) {
        g_string_append(out, "super+");
        p += 6;
      } else {
        while (*p != '\0' && *p != '>')
          g_string_append_c(out, g_ascii_tolower(*p++));
        if (*p == '>')
          p++;
        g_string_append_c(out, '+');
      }
    } else {
      g_string_append_c(out, g_ascii_tolower(*p));
      p++;
    }
  }

  if (out->len > 0 && out->str[out->len - 1] == '+')
    g_string_truncate(out, out->len - 1);

  return g_string_free(out, FALSE);
}
