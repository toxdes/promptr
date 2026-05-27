#ifndef WINDOW_H
#define WINDOW_H

#include "configfile.h"
#include "tab.h"
#include <gtk/gtk.h>

typedef struct _AppWindow {
  GtkApplication *app;
  GtkWidget *window;
  GtkWidget *cmd_label;

  GtkWidget *log_btn;
  GtkWidget *log_popup;
  GtkWidget *shortcuts_btn;
  GtkWidget *shortcuts_popup;
  GtkWidget *status_bar;
  FILE *log_file;

  gboolean destroyed;

  RuntimeConfig *config;
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
  guint kb_layout_keyval;
  GdkModifierType kb_layout_mods;
  guint kb_popout_keyval;
  GdkModifierType kb_popout_mods;
  gboolean esc_armed;
  guint esc_reset_timeout;

  GPtrArray *tabs;
  int active_tab_idx;
  GtkWidget *tab_bar;

  guint kb_new_tab_keyval;
  GdkModifierType kb_new_tab_mods;
  guint kb_close_tab_keyval;
  GdkModifierType kb_close_tab_mods;
  guint kb_restore_tab_keyval;
  GdkModifierType kb_restore_tab_mods;
  guint kb_follow_up_toggle_keyval;
  GdkModifierType kb_follow_up_toggle_mods;
} AppWindow;

AppWindow *app_window_new(GtkApplication *app);
void app_window_show(AppWindow *win);
void app_window_present(AppWindow *win);
void app_window_close_and_quit(AppWindow *win);
void app_window_free(gpointer data);
void app_window_save_state(AppWindow *win);

Tab *app_window_get_active_tab(AppWindow *win);

#endif
