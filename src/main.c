#include "window.h"
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

static void on_activate(GApplication *app, gpointer user_data) {
  AppWindow *win;

  (void)user_data;
  win = g_object_get_data(G_OBJECT(app), "window");
  if (win != NULL) {
    app_window_present(win);
    return;
  }
  win = app_window_new(GTK_APPLICATION(app));
  g_object_set_data_full(G_OBJECT(app), "window", win,
                         (GDestroyNotify)app_window_free);
  app_window_show(win);
}

static void on_shutdown(GApplication *app, gpointer user_data) {
  AppWindow *win;

  (void)user_data;
  win = g_object_get_data(G_OBJECT(app), "window");
  if (win != NULL) {
    app_window_save_state(win);
    app_window_close_and_quit(win);
    g_object_set_data(G_OBJECT(app), "window", NULL);
  }
}

int main(int argc, char *argv[]) {
  GtkApplication *app;
  int status;

  g_set_prgname("promptr");
  gtk_source_init();
  app = gtk_application_new("com.toxdes.promptr", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
