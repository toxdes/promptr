#include "tab.h"
#include "command.h"
#include "config.h"
#include "window.h"
#include <gio/gio.h>
#include <stdlib.h>

static char *tabs_dir(void) {
  const char *data_dir;
  g_autofree char *dir = NULL;

  data_dir = g_get_user_data_dir();
  dir = g_build_filename(data_dir, DATA_DIR_SUFFIX, "tabs", NULL);
  g_mkdir_with_parents(dir, 0700);
  return g_strdup(dir);
}

static char *tmp_root(void) {
  const char *data_dir;
  g_autofree char *dir = NULL;

  data_dir = g_get_user_data_dir();
  dir = g_build_filename(data_dir, DATA_DIR_SUFFIX, "tmp", NULL);
  g_mkdir_with_parents(dir, 0700);
  return g_strdup(dir);
}

Tab *tab_new(AppWindow *win, const char *name) {
  Tab *tab;

  tab = g_new0(Tab, 1);
  tab->win = win;
  tab->id = g_uuid_string_random();
  tab->name = g_strdup(name);
  tab->has_activity = FALSE;
  tab->is_open = TRUE;
  tab->state = 0;
  tab->ref_count = 1;

  {
    g_autofree char *root = NULL;

    root = tmp_root();
    tab->tmpdir_path = g_build_filename(root, tab->id, NULL);
  }

  return tab;
}

Tab *tab_ref(Tab *tab) {
  if (tab != NULL)
    g_atomic_int_inc(&tab->ref_count);
  return tab;
}

static void tab_free(Tab *tab) {
  if (tab == NULL)
    return;

  if (tab->subprocess != NULL)
    command_cancel(tab);

  g_list_free_full(tab->qa_history, g_free);
  g_free(tab->id);
  g_free(tab->name);
  g_free(tab->page_name);
  g_free(tab->tmpdir_path);
  g_free(tab->last_query);
  g_free(tab->last_output);
  g_free(tab->cmd_string);
  g_free(tab->marked_lines_str);

  g_free(tab);
}

void tab_unref(Tab *tab) {
  if (tab != NULL && g_atomic_int_dec_and_test(&tab->ref_count))
    tab_free(tab);
}

void remove_dir(const char *path) {
  GDir *dir;
  const char *name;
  g_autofree char *fpath = NULL;

  dir = g_dir_open(path, 0, NULL);
  if (dir != NULL) {
    while ((name = g_dir_read_name(dir)) != NULL) {
      fpath = g_build_filename(path, name, NULL);
      unlink(fpath);
      g_free(fpath);
      fpath = NULL;
    }
    g_dir_close(dir);
  }
  rmdir(path);
}

void tab_save(Tab *tab) {
  g_autofree char *dir = NULL;
  g_autofree char *conf_path = NULL;
  g_autofree char *txt_path = NULL;
  g_autofree char *data = NULL;
  g_autoptr(GKeyFile) kf = NULL;
  GList *l;
  int turn;
  char timestr[64];
  time_t now;
  struct tm *tm;

  if (tab == NULL || tab->id == NULL)
    return;

  dir = tabs_dir();
  conf_path = g_strdup_printf("%s/%s.conf", dir, tab->id);

  kf = g_key_file_new();
  g_key_file_set_integer(kf, "tab", "version", 1);
  g_key_file_set_string(kf, "tab", "name", tab->name);
  g_key_file_set_integer(kf, "tab", "layout", tab->layout_mode);
  g_key_file_set_boolean(kf, "tab", "is_open", tab->is_open);
  g_key_file_set_boolean(kf, "tab", "has_activity", tab->has_activity);

  {
    const char *agent, *model;

    agent = get_selected_text(tab->agent_dropdown);
    model = get_selected_text(tab->model_dropdown);
    if (agent != NULL)
      g_key_file_set_string(kf, "tab", "agent", agent);
    if (model != NULL)
      g_key_file_set_string(kf, "tab", "model", model);
    g_free((gpointer)agent);
    g_free((gpointer)model);
  }

  now = time(NULL);
  tm = localtime(&now);
  strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", tm);
  g_key_file_set_string(kf, "tab", "time", timestr);

  g_key_file_set_string(kf, "follow_up", "last_query",
                        tab->last_query != NULL ? tab->last_query : "");
  g_key_file_set_string(kf, "follow_up", "last_output",
                        tab->last_output != NULL ? tab->last_output : "");
  g_key_file_set_integer(kf, "follow_up", "turn", tab->follow_up_turn);

  turn = tab->follow_up_turn;
  for (l = tab->qa_history; l != NULL; l = l->next) {
    g_autofree char *group = NULL;
    const char *block;
    char *sep, *newline;
    g_autofree char *query = NULL;
    g_autofree char *output = NULL;

    block = (const char *)l->data;
    sep = strstr(block, ": ");
    newline = sep != NULL ? strstr(sep, "\nA: ") : NULL;
    if (sep != NULL && newline != NULL) {
      query = g_strndup(sep + 2, newline - sep - 2);
      output = g_strdup(newline + 4);
    } else {
      query = g_strdup(block);
      output = g_strdup("");
    }

    turn--;
    group = g_strdup_printf("history_%d", turn);
    g_key_file_set_string(kf, group, "query", query);
    g_key_file_set_string(kf, group, "output", output);
  }

  data = g_key_file_to_data(kf, NULL, NULL);
  g_file_set_contents(conf_path, data, -1, NULL);

  txt_path = g_strdup_printf("%s/%s.txt", dir, tab->id);
  if (tab->output_view != NULL && GTK_IS_TEXT_VIEW(tab->output_view)) {
    GtkTextBuffer *buf;
    GtkTextIter start, end;
    g_autofree char *text = NULL;

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
    gtk_text_buffer_get_bounds(buf, &start, &end);
    text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (text != NULL && text[0] != '\0')
      g_file_set_contents(txt_path, text, -1, NULL);
    else
      unlink(txt_path);
  }
}

Tab *tab_load(AppWindow *win, const char *uuid) {
  g_autofree char *dir = NULL;
  g_autofree char *conf_path = NULL;
  g_autofree char *txt_path = NULL;
  g_autoptr(GKeyFile) kf = NULL;
  g_autofree char *name = NULL;
  g_autofree char *agent = NULL;
  g_autofree char *model = NULL;
  g_autofree char *text = NULL;
  Tab *tab;
  int layout;
  int turn;
  int i;
  GtkWidget *page;
  GtkWidget *label_widget;

  dir = tabs_dir();
  conf_path = g_strdup_printf("%s/%s.conf", dir, uuid);

  kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, conf_path, G_KEY_FILE_NONE, NULL))
    return NULL;

  name = g_key_file_get_string(kf, "tab", "name", NULL);
  if (name == NULL)
    name = g_strdup("New Tab");

  tab = g_new0(Tab, 1);
  tab->win = win;
  tab->id = g_strdup(uuid);
  tab->name = g_strdup(name);
  tab->has_activity = TRUE;
  tab->is_open = TRUE;
  tab->state = 0;
  tab->ref_count = 1;

  {
    g_autofree char *root = NULL;

    root = tmp_root();
    tab->tmpdir_path = g_build_filename(root, tab->id, NULL);
  }

  layout = g_key_file_get_integer(kf, "tab", "layout", NULL);
  tab->layout_mode = layout;
  tab->marked_lines_str = g_strdup(runtime_config_get_string(
      win->config, "marked_lines", DEFAULT_MARKED_LINES_STR));

  agent = g_key_file_get_string(kf, "tab", "agent", NULL);
  model = g_key_file_get_string(kf, "tab", "model", NULL);

  g_ptr_array_add(win->tabs, tab);
  page = tab_create_widgets(tab, win);
  label_widget = tab_create_label(tab, win->tabs->len - 1, win);

  gtk_notebook_append_page(GTK_NOTEBOOK(win->tab_bar), page, label_widget);

  if (agent != NULL)
    set_selected_text(tab->agent_dropdown, agent);
  if (model != NULL)
    set_selected_text(tab->model_dropdown, model);

  txt_path = g_strdup_printf("%s/%s.txt", dir, uuid);
  if (g_file_get_contents(txt_path, &text, NULL, NULL) && text != NULL) {
    GtkTextBuffer *buf;

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
    gtk_text_buffer_set_text(buf, text, -1);
  }

  tab->follow_up_turn = g_key_file_get_integer(kf, "follow_up", "turn", NULL);
  {
    g_autofree char *lq = NULL;
    g_autofree char *lo = NULL;

    lq = g_key_file_get_string(kf, "follow_up", "last_query", NULL);
    lo = g_key_file_get_string(kf, "follow_up", "last_output", NULL);
    if (lq != NULL && lq[0] != '\0')
      tab->last_query = g_strdup(lq);
    if (lo != NULL && lo[0] != '\0')
      tab->last_output = g_strdup(lo);
  }

  turn = tab->follow_up_turn;
  for (i = 1; i <= turn; i++) {
    g_autofree char *group = NULL;
    g_autofree char *q = NULL;
    g_autofree char *o = NULL;
    int idx;
    char *block;

    idx = turn - i;
    group = g_strdup_printf("history_%d", idx);
    q = g_key_file_get_string(kf, group, "query", NULL);
    o = g_key_file_get_string(kf, group, "output", NULL);
    if (q != NULL && o != NULL) {
      block = g_strdup_printf("Q.%d: %s\nA: %s", idx, q, o);
      tab->qa_history = g_list_prepend(tab->qa_history, block);
    }
  }

  tab->follow_up = FALSE;
  tab->follow_up_active = FALSE;
  tab->defaults_applied = TRUE;

  if (g_file_test(tab->tmpdir_path, G_FILE_TEST_IS_DIR))
    gtk_widget_set_sensitive(tab->follow_up_check, TRUE);

  return tab;
}

void tab_delete_saved(const char *uuid) {
  g_autofree char *dir = NULL;
  g_autofree char *conf_path = NULL;
  g_autofree char *txt_path = NULL;

  dir = tabs_dir();
  conf_path = g_strdup_printf("%s/%s.conf", dir, uuid);
  txt_path = g_strdup_printf("%s/%s.txt", dir, uuid);
  unlink(conf_path);
  unlink(txt_path);
}

void tab_auto_rename(Tab *tab) {
  guint i;
  int n_pages;
  GtkNotebook *nb;

  if (tab == NULL)
    return;
  if (!g_str_has_prefix(tab->name, "New Tab"))
    return;

  {
    g_autofree char *prefix = g_strndup(tab->id, 8);
    g_autofree char *newname = NULL;

    newname = g_strdup_printf("New Tab %s", prefix);
    g_free(tab->name);
    tab->name = newname;
    newname = NULL;
  }

  nb = GTK_NOTEBOOK(tab->win->tab_bar);
  n_pages = gtk_notebook_get_n_pages(nb);
  for (i = 0; i < (guint)n_pages; i++) {
    Tab *t;
    GtkWidget *box, *child;

    t = g_ptr_array_index(tab->win->tabs, i);
    if (t == tab) {
      box =
          gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, (int)i));
      if (box != NULL) {
        child = gtk_widget_get_first_child(box);
        while (child != NULL) {
          if (GTK_IS_LABEL(child) &&
              !gtk_widget_has_css_class(child, "tab-status-dot") &&
              !gtk_widget_has_css_class(child, "tab-close-btn")) {
            gtk_label_set_text(GTK_LABEL(child), tab->name);
            gtk_widget_set_tooltip_text(child, tab->name);
            break;
          }
          child = gtk_widget_get_next_sibling(child);
        }
      }
      break;
    }
  }

  gtk_window_set_title(GTK_WINDOW(tab->win->window), tab->name);
}

Tab *app_window_get_active_tab(AppWindow *win) {
  if (win->tabs == NULL || win->tabs->len == 0)
    return NULL;
  return g_ptr_array_index(win->tabs, win->active_tab_idx);
}
