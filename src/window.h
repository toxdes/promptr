#ifndef WINDOW_H
#define WINDOW_H

#include "configfile.h"
#include <gtk/gtk.h>

typedef struct _AppWindow {
  GtkApplication *app;
  GtkWidget *window;
  GtkWidget *prompt_view;
  GtkWidget *placeholder_label;
  GtkWidget *agent_dropdown;
  GtkWidget *model_dropdown;
  GtkWidget *submit_btn;
  GtkWidget *cmd_label;
  GtkWidget *cancel_btn;
  GtkWidget *spinner;
  GtkWidget *output_scroll;
  GtkWidget *output_view;
  GtkWidget *copy_btn;
  GtkWidget *close_btn;
  GtkWidget *quit_btn;
  GtkWidget *marked_label;
  GtkWidget *output_label;
  GtkWidget *log_btn;
  GtkWidget *log_popup;
  GtkWidget *status_bar;
  GtkWidget *shortcuts_btn;
  GtkWidget *shortcuts_popup;
  FILE *log_file;

  GSubprocess *subprocess;
  GCancellable *cancellable;
  char *cmd_string;
  gint64 start_time;
  gboolean destroyed;
  int state;
  gboolean defaults_applied;
  GSList *temp_dirs;

  RuntimeConfig *config;
  char *marked_lines_str;
  char *opencode_bin;

  guint kb_focus_keyval;
  GdkModifierType kb_focus_mods;
  guint kb_copy_keyval;
  GdkModifierType kb_copy_mods;
  guint kb_close_keyval;
  GdkModifierType kb_close_mods;
  guint kb_quit_keyval;
  GdkModifierType kb_quit_mods;
  guint kb_log_keyval;
  GdkModifierType kb_log_mods;
  guint kb_shortcuts_keyval;
  GdkModifierType kb_shortcuts_mods;
  guint kb_submit_keyval;
  GdkModifierType kb_submit_mods;
  guint kb_cancel_keyval;
  GdkModifierType kb_cancel_mods;
  gboolean esc_armed;
  guint esc_reset_timeout;

  GtkWidget *follow_up_check;
  gboolean follow_up;
  gboolean follow_up_active;
  char *last_tmpdir;
} AppWindow;

AppWindow *app_window_new(GtkApplication *app);
void app_window_show(AppWindow *win);
void app_window_present(AppWindow *win);
void app_window_close_and_quit(AppWindow *win);
void app_window_free(gpointer data);
void app_window_save_state(AppWindow *win);

#endif
