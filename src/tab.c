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

  dir = g_dir_open(path, 0, NULL);
  if (dir != NULL) {
    while ((name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *fpath = g_build_filename(path, name, NULL);

      if (g_file_test(fpath, G_FILE_TEST_IS_DIR))
        remove_dir(fpath);
      else
        unlink(fpath);
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
  struct tm tm;

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
  g_key_file_set_boolean(kf, "tab", "follow_up", tab->follow_up);

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
  localtime_r(&now, &tm);
  strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", &tm);
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

  {
    g_autofree char *prompt_path = NULL;
    g_autofree char *prompt_text = NULL;

    prompt_path = g_strdup_printf("%s/%s.prompt", dir, tab->id);
    if (tab->prompt_view != NULL && GTK_IS_TEXT_VIEW(tab->prompt_view)) {
      GtkTextBuffer *buf;
      GtkTextIter start, end;

      buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
      gtk_text_buffer_get_bounds(buf, &start, &end);
      prompt_text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    }
    if (prompt_text != NULL && prompt_text[0] != '\0')
      g_file_set_contents(prompt_path, prompt_text, -1, NULL);
    else
      unlink(prompt_path);
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

  page = tab_create_widgets(tab, win);
  g_ptr_array_add(win->tabs, tab);
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

  tab->follow_up = g_key_file_get_boolean(kf, "tab", "follow_up", NULL);
  tab->follow_up_active = FALSE;
  tab->defaults_applied = TRUE;

  if (g_file_test(tab->tmpdir_path, G_FILE_TEST_IS_DIR)) {
    gtk_widget_set_sensitive(tab->follow_up_check, TRUE);
    if (tab->follow_up)
      gtk_check_button_set_active(GTK_CHECK_BUTTON(tab->follow_up_check), TRUE);
  }

  {
    g_autofree char *prompt_path = NULL;
    g_autofree char *prompt_text = NULL;

    prompt_path = g_strdup_printf("%s/%s.prompt", dir, tab->id);
    if (g_file_get_contents(prompt_path, &prompt_text, NULL, NULL) &&
        prompt_text != NULL && prompt_text[0] != '\0') {
      GtkTextBuffer *buf;

      buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
      gtk_text_buffer_set_text(buf, prompt_text, -1);
    }
  }

  return tab;
}

void tab_delete_saved(const char *uuid) {
  g_autofree char *dir = NULL;
  g_autofree char *conf_path = NULL;
  g_autofree char *txt_path = NULL;
  g_autofree char *prompt_path = NULL;

  dir = tabs_dir();
  conf_path = g_strdup_printf("%s/%s.conf", dir, uuid);
  txt_path = g_strdup_printf("%s/%s.txt", dir, uuid);
  prompt_path = g_strdup_printf("%s/%s.prompt", dir, uuid);
  unlink(conf_path);
  unlink(txt_path);
  unlink(prompt_path);
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
    g_autofree char *text = NULL;
    g_autoptr(GString) clean = NULL;

    if (tab->prompt_view != NULL && GTK_IS_TEXT_VIEW(tab->prompt_view)) {
      GtkTextBuffer *buf;
      GtkTextIter start, end_iter;

      buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
      gtk_text_buffer_get_bounds(buf, &start, &end_iter);
      text = gtk_text_buffer_get_text(buf, &start, &end_iter, FALSE);
    }

    if (text != NULL) {
      const char *p;
      gboolean non_space;

      clean = g_string_new(NULL);
      non_space = FALSE;
      for (p = text; *p != '\0' && *p != '\n' && clean->len < 200;
           p = g_utf8_next_char(p)) {
        gunichar uc;

        uc = g_utf8_get_char(p);
        if (g_unichar_iscntrl(uc))
          continue;
        if (!non_space && g_unichar_isspace(uc))
          continue;
        non_space = TRUE;
        g_string_append_unichar(clean, uc);
      }

      while (clean->len > 0 && g_ascii_isspace(clean->str[clean->len - 1]))
        g_string_truncate(clean, clean->len - 1);

      if (clean->len > 0) {
        g_free(tab->name);
        tab->name = g_string_free(clean, FALSE);
        clean = NULL;
      }
    }
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
  if (win->active_tab_idx < 0 || win->active_tab_idx >= (int)win->tabs->len)
    return NULL;
  return g_ptr_array_index(win->tabs, win->active_tab_idx);
}

char *get_selected_text(GtkWidget *dropdown) {
  guint pos;
  GListModel *model;
  GObject *item;
  const char *str;

  pos = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
  if (pos == GTK_INVALID_LIST_POSITION)
    return g_strdup("None");

  model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  item = g_list_model_get_item(model, pos);
  if (item == NULL)
    return g_strdup("None");

  str = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
  char *result = g_strdup(str != NULL ? str : "None");
  g_object_unref(item);
  return result;
}

void set_selected_text(GtkWidget *dropdown, const char *text) {
  guint i;
  GListModel *model;
  guint n;

  model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  n = g_list_model_get_n_items(model);
  for (i = 0; i < n; i++) {
    GObject *item;
    const char *str;

    item = g_list_model_get_item(model, i);
    str = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
    if (g_strcmp0(str, text) == 0) {
      gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
      g_object_unref(item);
      return;
    }
    g_object_unref(item);
  }
}
