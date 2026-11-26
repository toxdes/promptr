#include "tab.h"
#include "config.h"
#include "provider-manager.h"
#include "provider-profile.h"
#include "window.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <string.h>

static char *tab_path(const char *id, const char *suffix) {
  g_autofree char *dir = thread_store_directory();
  g_autofree char *name = g_strconcat(id, suffix, NULL);
  return g_build_filename(dir, name, NULL);
}

GKeyFile *tab_read_metadata(const char *uuid) {
  if (!g_uuid_string_is_valid(uuid))
    return NULL;
  g_autofree char *path = tab_path(uuid, ".meta");
  GKeyFile *kf = g_key_file_new();
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    return kf;
  g_free(path);
  path = tab_path(uuid, ".conf");
  FILE *file = g_fopen(path, "r");
  if (file == NULL) {
    g_key_file_unref(kf);
    return NULL;
  }
  /* Legacy messages share this file: read only the lightweight [tab] section.
   */
  g_autoptr(GString) meta = g_string_new(NULL);
  char line[8192];
  gboolean in_tab = FALSE;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '[') {
      if (in_tab)
        break;
      in_tab = g_str_has_prefix(line, "[tab]");
    }
    if (in_tab)
      g_string_append(meta, line);
  }
  fclose(file);
  if (!g_key_file_load_from_data(kf, meta->str, meta->len, G_KEY_FILE_NONE,
                                 NULL)) {
    g_key_file_unref(kf);
    return NULL;
  }
  return kf;
}

static Tab *tab_allocate(AppWindow *win, const char *name,
                         gboolean choose_profile) {
  Tab *tab = g_new0(Tab, 1);
  tab->win = win;
  tab->id = g_uuid_string_random();
  tab->name = g_strdup(name);
  tab->is_open = TRUE;
  tab->ref_count = 1;
  tab->history_cursor = -1;
  tab->provider_name =
      runtime_config_get_string(win->config, "provider", PROVIDER_DEFAULT);
  if (choose_profile) {
    g_autoptr(GPtrArray) profiles = provider_profile_list();
    if (!g_uuid_string_is_valid(tab->provider_name)) {
      const char *preferred =
          *tab->provider_name ? tab->provider_name : PROVIDER_DEFAULT;
      ProviderProfile *fallback = NULL;
      for (guint i = 0; i < profiles->len; i++) {
        ProviderProfile *p = g_ptr_array_index(profiles, i);
        if (!p->tested && !p->bundled)
          continue;
        if (fallback == NULL)
          fallback = p;
        if (g_strcmp0(p->plugin, preferred) == 0) {
          fallback = p;
          break;
        }
      }
      if (fallback != NULL) {
        g_free(tab->provider_name);
        tab->provider_name = g_strdup(fallback->id);
      }
    }
  }
  tab->selected_agent = g_strdup("None");
  g_autoptr(ProviderProfile) selected =
      choose_profile ? provider_profile_load(tab->provider_name, NULL) : NULL;
  tab->selected_model =
      g_strdup(selected != NULL && selected->model != NULL && *selected->model
                   ? selected->model
                   : "None");
  tab->draft = g_strdup("");
  tab->marked_lines_str = runtime_config_get_string(win->config, "marked_lines",
                                                    DEFAULT_MARKED_LINES_STR);
  return tab;
}

Tab *tab_new(AppWindow *win, const char *name) {
  return tab_allocate(win, name, TRUE);
}

Tab *tab_ref(Tab *tab) {
  if (tab != NULL)
    g_atomic_int_inc(&tab->ref_count);
  return tab;
}

void tab_capture_input(Tab *tab) {
  if (!tab->hydrated || tab->restoring_draft)
    return;
  if (GTK_IS_TEXT_VIEW(tab->prompt_view)) {
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    g_free(tab->draft);
    tab->draft = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (tab->draft != NULL && *tab->draft)
      tab->has_activity = TRUE;
  }
  g_free(tab->selected_agent);
  tab->selected_agent = get_selected_text(tab->agent_dropdown);
  g_free(tab->selected_model);
  tab->selected_model = get_selected_text(tab->model_dropdown);
}

void tab_release_view(Tab *tab) {
  if (!tab->hydrated)
    return;
  tab_capture_input(tab);
  tab->hydrated = FALSE;
  tab->view_generation++;
  tab->loading_history = FALSE;
  GtkWidget *child = gtk_widget_get_first_child(tab->page);
  if (child != NULL)
    gtk_box_remove(GTK_BOX(tab->page), child);
  GtkWidget *owned[] = {tab->prompt_section, tab->fu_row,     tab->agent_row,
                        tab->output_section, tab->marked_row, tab->action_row,
                        tab->pane_left,      tab->pane_right, tab->copy_btn};
  for (guint i = 0; i < G_N_ELEMENTS(owned); i++)
    if (owned[i] != NULL)
      g_object_unref(owned[i]);
  tab->prompt_view = NULL;
  tab->output_view = NULL;
  tab->agent_dropdown = NULL;
  tab->model_dropdown = NULL;
  tab->content_stack = NULL;
  tab->prompt_section = NULL;
  tab->output_section = NULL;
  tab->fu_row = NULL;
  tab->agent_row = NULL;
  tab->marked_row = NULL;
  tab->action_row = NULL;
  tab->pane_left = NULL;
  tab->pane_right = NULL;
  tab->paned_positioned = FALSE;
  tab->copy_btn = NULL;
  g_clear_pointer(&tab->draft, g_free);
  if (tab->state != STATE_LOADING && !tab->storage_failed) {
    thread_store_unref(tab->store);
    tab->store = NULL;
  }
}

static void tab_free(Tab *tab) {
  if (tab->provider != NULL)
    provider_manager_destroy(tab->provider);
  g_clear_object(&tab->cancellable);
  if (tab->hydrated)
    tab_release_view(tab);
  thread_store_unref(tab->store);
  g_free(tab->id);
  g_free(tab->name);
  g_free(tab->provider_name);
  g_free(tab->cmd_string);
  g_free(tab->marked_lines_str);
  g_free(tab->selected_agent);
  g_free(tab->selected_model);
  g_free(tab->draft);
  g_free(tab->project_dir);
  g_free(tab->request_id);
  g_free(tab->request_profile);
  g_free(tab);
}

void tab_unref(Tab *tab) {
  if (tab != NULL && g_atomic_int_dec_and_test(&tab->ref_count))
    tab_free(tab);
}

static gboolean tab_save_marks(Tab *tab, GError **error) {
  if (!tab->hydrated || tab->loading_history || tab->state == STATE_LOADING ||
      !GTK_IS_TEXT_VIEW(tab->output_view))
    return TRUE;
  GtkTextBuffer *buffer =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  GtkTextIter start, end, iter;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (text == NULL || !*text)
    return TRUE;
  g_autofree char *hash =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
  g_autoptr(GString) marks = g_string_new(NULL);
  iter = start;
  GSList *first = gtk_source_buffer_get_source_marks_at_iter(
      GTK_SOURCE_BUFFER(buffer), &iter, "promptr-mark");
  if (first != NULL)
    g_string_append(marks, "1");
  g_slist_free(first);
  while (gtk_source_buffer_forward_iter_to_source_mark(
      GTK_SOURCE_BUFFER(buffer), &iter, "promptr-mark")) {
    if (marks->len > 0)
      g_string_append_c(marks, ',');
    g_string_append_printf(marks, "%d", gtk_text_iter_get_line(&iter) + 1);
  }
  g_autofree char *path = tab_path(tab->id, ".marks");
  g_autoptr(GKeyFile) saved = g_key_file_new();
  g_key_file_load_from_file(saved, path, G_KEY_FILE_NONE, NULL);
  g_autofree char *previous = g_key_file_get_string(saved, "marks", hash, NULL);
  const char *selection = marks->len > 0 ? marks->str : "-1";
  if (g_strcmp0(previous, selection) == 0)
    return TRUE;
  g_key_file_set_string(saved, "marks", hash, selection);
  g_autofree char *data = g_key_file_to_data(saved, NULL, NULL);
  g_autofree char *dir = thread_store_directory();
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Save output marks: %s", g_strerror(errno));
    return FALSE;
  }
  return g_file_set_contents_full(path, data, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

gboolean tab_save(Tab *tab) {
  if (tab == NULL || tab->id == NULL)
    return FALSE;
  tab_capture_input(tab);
  g_autofree char *dir = thread_store_directory();
  g_autofree char *path = tab_path(tab->id, ".meta");
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_key_file_set_integer(kf, "tab", "version", 3);
  g_key_file_set_integer(kf, "tab", "state", tab->state);
  g_key_file_set_string(kf, "tab", "name", tab->name);
  g_key_file_set_boolean(kf, "tab", "is_open", tab->is_open);
  g_key_file_set_boolean(kf, "tab", "has_activity",
                         tab->has_activity ||
                             (tab->draft != NULL && *tab->draft));
  g_key_file_set_boolean(kf, "tab", "follow_up", tab->follow_up);
  g_key_file_set_boolean(kf, "tab", "unread", tab->has_unseen_output);
  g_key_file_set_integer(kf, "tab", "layout", tab->layout_mode);
  g_key_file_set_string(kf, "tab", "provider",
                        tab->provider_name != NULL ? tab->provider_name : "");
  g_key_file_set_string(kf, "tab", "agent",
                        tab->selected_agent != NULL ? tab->selected_agent
                                                    : "None");
  g_key_file_set_string(kf, "tab", "model",
                        tab->selected_model != NULL ? tab->selected_model
                                                    : "None");
  g_key_file_set_string(kf, "tab", "project",
                        tab->project_dir != NULL ? tab->project_dir : "");
  g_autoptr(GDateTime) now = g_date_time_new_now_utc();
  g_autofree char *time = g_date_time_format_iso8601(now);
  g_key_file_set_string(kf, "tab", "time", time);
  g_autofree char *data = g_key_file_to_data(kf, NULL, NULL);
  g_autoptr(GError) error = NULL;
  if (!tab_save_marks(tab, &error)) {
    g_warning("Could not save output marks: %s", error->message);
    tab->storage_failed = TRUE;
    return FALSE;
  }
  if (tab->hydrated && !tab->restoring_draft) {
    g_autofree char *prompt_path = tab_path(tab->id, ".prompt");
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents_full(
            prompt_path, tab->draft != NULL ? tab->draft : "", -1,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600,
            &error)) {
      g_warning("Could not save draft: %s",
                error != NULL ? error->message : g_strerror(errno));
      tab->storage_failed = TRUE;
      return FALSE;
    }
  }
  if (g_mkdir_with_parents(dir, 0700) != 0 ||
      !g_file_set_contents_full(path, data, -1,
                                G_FILE_SET_CONTENTS_CONSISTENT |
                                    G_FILE_SET_CONTENTS_DURABLE,
                                0600, &error)) {
    g_warning("Could not save tab metadata: %s",
              error != NULL ? error->message : g_strerror(errno));
    tab->storage_failed = TRUE;
    return FALSE;
  }
  tab->storage_failed = thread_store_has_unsaved(tab->store);
  g_clear_pointer(&tab->draft, g_free);
  return TRUE;
}

Tab *tab_load(AppWindow *win, const char *uuid) {
  if (thread_store_is_deleting(uuid))
    return NULL;
  g_autoptr(GKeyFile) kf = tab_read_metadata(uuid);
  if (kf == NULL)
    return NULL;
  g_autofree char *name = g_key_file_get_string(kf, "tab", "name", NULL);
  Tab *tab = tab_allocate(win, name != NULL ? name : "New Tab", FALSE);
  g_free(tab->id);
  tab->id = g_strdup(uuid);
  tab->has_activity = g_key_file_get_boolean(kf, "tab", "has_activity", NULL);
  tab->follow_up = g_key_file_get_boolean(kf, "tab", "follow_up", NULL);
  tab->layout_mode = g_key_file_get_integer(kf, "tab", "layout", NULL);
  int state = g_key_file_get_integer(kf, "tab", "state", NULL);
  tab->state =
      state >= STATE_IDLE && state <= STATE_FAILED ? state : STATE_IDLE;
  gboolean interrupted = tab->state == STATE_LOADING;
  if (interrupted)
    tab->state = STATE_FAILED;
  tab->has_unseen_output = g_key_file_get_boolean(kf, "tab", "unread", NULL);
  tab->has_unseen_output = tab->has_unseen_output || interrupted;
  g_autofree char *provider =
      g_key_file_get_string(kf, "tab", "provider", NULL);
  if (provider != NULL) {
    g_free(tab->provider_name);
    tab->provider_name = g_steal_pointer(&provider);
  }
  g_free(tab->selected_agent);
  tab->selected_agent = g_key_file_get_string(kf, "tab", "agent", NULL);
  g_free(tab->selected_model);
  tab->selected_model = g_key_file_get_string(kf, "tab", "model", NULL);
  tab->project_dir = g_key_file_get_string(kf, "tab", "project", NULL);
  g_clear_pointer(&tab->draft, g_free);
  tab->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_ptr_array_add(win->tabs, tab);
  GtkWidget *label = tab_create_label(tab, win->tabs->len - 1, win);
  gtk_notebook_append_page(GTK_NOTEBOOK(win->tab_bar), tab->page, label);
  return tab;
}

void tab_delete_saved(const char *uuid) {
  g_autoptr(GError) error = NULL;
  if (!thread_store_delete(uuid, &error) && error != NULL)
    g_warning("Delete history: %s", error->message);
}

void tab_auto_rename(Tab *tab) {

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

  if (tab->win == NULL)
    return;

  if (GTK_IS_LABEL(tab->tab_name_label)) {
    gtk_label_set_text(GTK_LABEL(tab->tab_name_label), tab->name);
    gtk_widget_set_tooltip_text(tab->tab_name_label, tab->name);
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

  if (!GTK_IS_DROP_DOWN(dropdown))
    return g_strdup("None");
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

  if (!GTK_IS_DROP_DOWN(dropdown) || text == NULL)
    return;
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
  if (GTK_IS_STRING_LIST(model)) {
    gtk_string_list_append(GTK_STRING_LIST(model), text);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), n);
  }
}
