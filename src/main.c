#include "config.h"
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

  {
    const char *cfg_dir;
    g_autofree char *cfg_path = NULL;
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *renderer = NULL;

    cfg_dir = g_get_user_config_dir();
    cfg_path = g_build_filename(cfg_dir, "promptr", "config", NULL);
    if (g_key_file_load_from_file(kf, cfg_path, G_KEY_FILE_NONE, NULL)) {
      renderer = g_key_file_get_string(kf, "preferences", "gsk_renderer", NULL);
    }
    if (renderer == NULL || renderer[0] == '\0')
      renderer = g_strdup(GSK_RENDERER_DEFAULT);
    g_setenv("GSK_RENDERER", renderer, FALSE);
  }

  g_set_prgname(DATA_DIR_SUFFIX);
  gtk_source_init();
  app = gtk_application_new("com.toxdes.promptr", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
