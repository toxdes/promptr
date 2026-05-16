#ifndef WINDOW_H
#define WINDOW_H

#include <gtk/gtk.h>

typedef struct _AppWindow {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *prompt_view;
    GtkWidget      *placeholder_label;
    GtkWidget      *agent_dropdown;
    GtkWidget      *model_dropdown;
    GtkWidget      *submit_btn;
    GtkWidget      *cmd_label;
    GtkWidget      *cancel_btn;
    GtkWidget      *output_scroll;
    GtkWidget      *output_view;
    GtkWidget      *copy_btn;
    GtkWidget      *close_btn;
    GtkWidget      *quit_btn;
    GtkWidget      *marked_label;

    GSubprocess    *subprocess;
    GCancellable   *cancellable;
    char           *cmd_string;
    gint64          start_time;
    gboolean        destroyed;
    int             state;

    guint            kb_focus_keyval;
    GdkModifierType  kb_focus_mods;
    guint            kb_copy_keyval;
    GdkModifierType  kb_copy_mods;
    guint            kb_quit_keyval;
    GdkModifierType  kb_quit_mods;
} AppWindow;

AppWindow *app_window_new(GtkApplication *app);
void       app_window_show(AppWindow *win);
void       app_window_present(AppWindow *win);
void       app_window_close_and_quit(AppWindow *win);
void       app_window_free(gpointer data);
void       app_window_save_state(AppWindow *win);

#endif
