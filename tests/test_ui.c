/* Native GTK integration checks. Run only with explicit permission to launch
 * Promptr's UI, through run_ui.py with isolated XDG directories. */
#include "../config.h"
#include "../src/app-diagnostics.h"
#include "../src/provider-profile.h"
#include "../src/window.h"
#include <gtksourceview/gtksource.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static void pump(void) {
  while (g_main_context_iteration(NULL, FALSE)) {
  }
}
static void settle(guint milliseconds) {
  gint64 until = g_get_monotonic_time() + milliseconds * 1000;
  do {
    pump();
    g_usleep(1000);
  } while (g_get_monotonic_time() < until);
}
static GtkWidget *find_button(GtkWidget *widget, const char *label) {
  if (GTK_IS_BUTTON(widget) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_button(child, label);
    if (found != NULL)
      return found;
  }
  return NULL;
}
static GtkWidget *find_entry(GtkWidget *widget, const char *label) {
  if (GTK_IS_LABEL(widget) &&
      g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), label) == 0) {
    GtkWidget *next = gtk_widget_get_next_sibling(widget);
    if (GTK_IS_ENTRY(next))
      return next;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_entry(child, label);
    if (found != NULL)
      return found;
  }
  return NULL;
}
static GtkWidget *find_list(GtkWidget *widget) {
  if (GTK_IS_LIST_BOX(widget) && gtk_widget_get_mapped(widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_list(child);
    if (found != NULL)
      return found;
  }
  return NULL;
}
static GtkWidget *find_open_popover(GtkWidget *widget) {
  if (GTK_IS_POPOVER(widget) && gtk_widget_get_mapped(widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_open_popover(child);
    if (found != NULL)
      return found;
  }
  return NULL;
}
static GtkWidget *find_choice(GtkWidget *widget, const char *value,
                              guint *position) {
  if (GTK_IS_DROP_DOWN(widget)) {
    GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(widget));
    for (guint i = 0; model != NULL && i < g_list_model_get_n_items(model);
         i++) {
      g_autoptr(GObject) item = g_list_model_get_item(model, i);
      if (GTK_IS_STRING_OBJECT(item) &&
          g_strcmp0(gtk_string_object_get_string(GTK_STRING_OBJECT(item)),
                    value) == 0) {
        *position = i;
        return widget;
      }
    }
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_choice(child, value, position);
    if (found != NULL)
      return found;
  }
  return NULL;
}
static GtkWidget *find_window(const char *title) {
  GListModel *windows = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    GtkWindow *window = g_list_model_get_item(windows, i);
    if (g_strcmp0(gtk_window_get_title(window), title) == 0)
      return GTK_WIDGET(window);
    g_object_unref(window);
  }
  return NULL;
}
static void action(AppWindow *win, const char *name) {
  g_assert_true(gtk_widget_activate_action(win->window, name, NULL));
}
static void wait_history(Tab *tab) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (tab->loading_history) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
}
static void prompt(Tab *tab, const char *text) {
  wait_history(tab);
  gtk_text_buffer_set_text(
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view)), text, -1);
}
static char *output(Tab *tab) {
  GtkTextBuffer *buffer =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}
static guint64 rss(void) {
  g_autofree char *data = NULL;
  if (!g_file_get_contents("/proc/self/status", &data, NULL, NULL))
    return 0;
  const char *line = strstr(data, "VmRSS:");
  guint64 value = 0;
  if (line != NULL)
    sscanf(line, "VmRSS: %" G_GUINT64_FORMAT, &value);
  return value;
}
static void screenshot(GtkWidget *window, const char *path) {
  g_print("Screenshot: %s\n", path);
  settle(150);
  int width = gtk_widget_get_width(window),
      height = gtk_widget_get_height(window);
  g_assert_cmpint(width, >, 0);
  g_assert_cmpint(height, >, 0);
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  g_autoptr(GskRenderNode) node = NULL;
  for (guint attempt = 0; node == NULL && attempt < 20; attempt++) {
    gtk_widget_queue_draw(window);
    settle(100);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    node = gtk_snapshot_free_to_node(snapshot);
  }
  if (node == NULL) {
    g_printerr("Screenshot unavailable (no rendered frame): %s\n", path);
    return;
  }
  graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(
      gtk_native_get_renderer(GTK_NATIVE(window)), node, &viewport);
  g_assert_nonnull(texture);
  g_assert_true(gdk_texture_save_to_png(texture, path));
}

static GtkWidget *find_type(GtkWidget *widget, GType type) {
  if (g_type_is_a(G_OBJECT_TYPE(widget), type) && gtk_widget_get_mapped(widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_type(child, type);
    if (found != NULL)
      return found;
  }
  return NULL;
}

static void test_model_search(AppWindow *win, const char *artifact_dir) {
  Tab *tab = app_window_get_active_tab(win);
  GtkDropDown *picker = GTK_DROP_DOWN(tab->model_dropdown);
  g_assert_true(gtk_drop_down_get_enable_search(picker));
  g_autoptr(GListModel) original =
      g_object_ref(gtk_drop_down_get_model(picker));
  g_autofree char *selection = get_selected_text(GTK_WIDGET(picker));
  const char *values[] = {"None", "fixture/paid", "fixture/free-one",
                          "fixture/another-free", NULL};
  g_autoptr(GtkStringList) models = gtk_string_list_new(values);
  gtk_drop_down_set_model(picker, G_LIST_MODEL(models));
  gtk_widget_set_sensitive(GTK_WIDGET(picker), TRUE);
  g_assert_true(gtk_widget_activate(GTK_WIDGET(picker)));
  GtkWidget *popover = find_open_popover(GTK_WIDGET(picker));
  gint64 popup_deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
  while (popover == NULL && g_get_monotonic_time() < popup_deadline) {
    pump();
    g_usleep(1000);
    popover = find_open_popover(GTK_WIDGET(picker));
  }
  g_assert_nonnull(popover);
  GtkWidget *search = find_type(popover, GTK_TYPE_SEARCH_ENTRY);
  g_assert_nonnull(search);
  gtk_editable_set_text(GTK_EDITABLE(search), "free");
  settle(700);
  GtkWidget *list = find_type(popover, GTK_TYPE_LIST_VIEW);
  gint64 list_deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (list == NULL && g_get_monotonic_time() < list_deadline) {
    if (!gtk_widget_get_mapped(popover)) {
      gtk_widget_activate(GTK_WIDGET(picker));
      settle(150);
      gtk_editable_set_text(GTK_EDITABLE(search), "free");
    }
    settle(150);
    list = find_type(popover, GTK_TYPE_LIST_VIEW);
  }
  g_assert_nonnull(list);
  GtkSelectionModel *filtered = gtk_list_view_get_model(GTK_LIST_VIEW(list));
  g_autofree char *path =
      g_build_filename(artifact_dir, "model-search.png", NULL);
  screenshot(win->window, path);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(filtered)), ==, 2);
  gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(filtered), 0);
  g_signal_emit_by_name(list, "activate", 0u);
  settle(150);
  g_autofree char *chosen = get_selected_text(GTK_WIDGET(picker));
  g_assert_cmpstr(chosen, ==, "fixture/free-one");
  gtk_popover_popdown(GTK_POPOVER(popover));
  gtk_drop_down_set_model(picker, original);
  set_selected_text(GTK_WIDGET(picker), selection);
}

static void emit_double_click(GtkGestureClick *gesture, double x, double y) {
  g_signal_emit_by_name(gesture, "pressed", 1, x, y);
  g_signal_emit_by_name(gesture, "pressed", 2, x, y);
}

static void test_connection_error_details(AppWindow *win,
                                          const char *artifact_dir) {
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) preferences = find_window("Preferences");
  GtkWidget *sections = find_type(preferences, GTK_TYPE_NOTEBOOK);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 2);
  settle(100);
  g_signal_emit_by_name(find_button(preferences, "Add provider"), "clicked");
  guint position;
  GtkWidget *plugins = find_choice(preferences, "custom", &position);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(plugins), position);
  gtk_editable_set_text(GTK_EDITABLE(find_entry(preferences, "Profile name")),
                        "Error fixture");
  g_autofree char *python = g_find_program_in_path("python3");
  gtk_editable_set_text(
      GTK_EDITABLE(find_entry(preferences, "Plugin executable")), python);
  g_autofree char *quoted = g_shell_quote(g_getenv("PROMPTR_TEST_FIXTURE"));
  g_autofree char *arguments = g_strdup_printf("%s long_error", quoted);
  gtk_editable_set_text(GTK_EDITABLE(find_entry(preferences, "Arguments")),
                        arguments);
  settle(100);
  int height = gtk_widget_get_height(preferences);
  g_signal_emit_by_name(find_button(preferences, "Test connection"), "clicked");
  g_autoptr(GtkWidget) dialog = NULL;
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while ((dialog = find_window("Connection test failed")) == NULL) {
    settle(50);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  settle(100);
  g_assert_true(gtk_window_get_modal(GTK_WINDOW(dialog)));
  g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(dialog)) ==
                GTK_WINDOW(preferences));
  GtkWidget *details = find_type(dialog, GTK_TYPE_TEXT_VIEW);
  g_assert_nonnull(details);
  g_assert_false(gtk_text_view_get_editable(GTK_TEXT_VIEW(details)));
  g_assert_nonnull(gtk_widget_get_ancestor(details, GTK_TYPE_SCROLLED_WINDOW));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(details));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_select_range(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_nonnull(strstr(text, "Backend diagnostic line 50:"));
  g_assert_cmpint(gtk_text_buffer_get_line_count(buffer), >=, 51);
  g_assert_true(gtk_text_buffer_get_has_selection(buffer));
  g_assert_cmpint(gtk_widget_get_height(preferences), ==, height);
  g_assert_cmpint(gtk_widget_get_height(dialog), <=, 500);
  g_autofree char *path =
      g_build_filename(artifact_dir, "connection-error.png", NULL);
  screenshot(dialog, path);
  gtk_window_destroy(GTK_WINDOW(dialog));
  g_signal_emit_by_name(find_button(preferences, "Cancel"), "clicked");
  settle(100);
  g_print("Long connection errors stay scrollable above Preferences\n");
}

static void test_model_selection(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);
  g_autoptr(ProviderProfile) profile =
      provider_profile_load(tab->provider_name, NULL);
  g_assert_null(json_object_get_member(profile->settings, "api_key"));
  char **models = g_new0(char *, 601);
  for (guint i = 0; i < 600; i++)
    models[i] = g_strdup_printf("fixture/model_%04u:free", i);
  g_hash_table_replace(win->model_catalogs, g_strdup(tab->provider_name),
                       models);
  g_print("Loading large model catalog\n");
  app_window_providers_changed(win);
  settle(150);
  g_print("Selecting model in dropdown\n");
  set_selected_text(tab->model_dropdown, "fixture/model_0500:free");
  settle(150);
  g_assert_cmpstr(tab->selected_model, ==, "fixture/model_0500:free");
  g_print("Selecting model in menu\n");
  g_assert_true(gtk_widget_activate_action(win->window, "win.cycle_model_set",
                                           "s", "fixture/model_0599:free"));
  settle(150);
  g_assert_cmpstr(tab->selected_model, ==, "fixture/model_0599:free");
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) preferences = find_window("Preferences");
  g_assert_nonnull(preferences);
  GtkWidget *sections = find_type(preferences, GTK_TYPE_NOTEBOOK);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 2);
  settle(100);
  guint position;
  GtkWidget *picker =
      find_choice(preferences, "fixture/model_0500:free", &position);
  g_assert_nonnull(picker);
  g_print("Selecting first Preferences model\n");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(picker), position);
  settle(100);
  GtkWidget *advanced = find_type(preferences, GTK_TYPE_EXPANDER);
  g_assert_nonnull(advanced);
  gtk_expander_set_expanded(GTK_EXPANDER(advanced), TRUE);
  settle(100);
  GtkWidget *model_entry = find_entry(preferences, "Model ID");
  g_assert_true(GTK_IS_EDITABLE(model_entry));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(model_entry)), ==,
                  "fixture/model_0500:free");
  g_assert_nonnull(
      find_choice(preferences, "fixture/model_0599:free", &position));
  g_print("Selecting second Preferences model\n");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(picker), position);
  settle(100);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(model_entry)), ==,
                  "fixture/model_0599:free");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(picker), 0);
  settle(100);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(model_entry)), ==, "");
  gtk_editable_set_text(GTK_EDITABLE(model_entry), "fixture/manual-model");
  settle(100);
  g_autofree char *selected = get_selected_text(picker);
  g_assert_cmpstr(selected, ==, "fixture/manual-model");
  g_print("Preferences selection finished\n");
  GtkWidget *cancel = find_button(preferences, "Cancel");
  g_signal_emit_by_name(cancel, "clicked");
  settle(100);
  g_print("Model selection without a key passed\n");
}

static void test_audit_regressions(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);
  wait_history(tab);
  for (guint i = 0; i < 120; i++) {
    g_assert_true(
        thread_store_begin(tab->store, "audit", "question", i == 0, NULL));
    g_assert_true(thread_store_chunk(tab->store, "answer", NULL));
    g_assert_true(thread_store_finish(tab->store, "completed", NULL));
  }
  action(win, "win.latest_history");
  wait_history(tab);
  guint pages = 0;
  while (tab->history_more) {
    action(win, "win.older_history");
    wait_history(tab);
    g_assert_cmpuint(++pages, <, 10);
  }
  g_assert_cmpuint(pages, >, 0);
  g_autofree char *oldest = output(tab);
  g_action_group_activate_action(G_ACTION_GROUP(win->actions), "older_history",
                                 NULL);
  settle(100);
  g_autofree char *unchanged = output(tab);
  g_assert_cmpstr(unchanged, ==, oldest);
  g_assert_false(g_action_group_get_action_enabled(G_ACTION_GROUP(win->actions),
                                                   "older_history"));
  g_autofree char *id = g_strdup(tab->id);
  tab->has_activity = TRUE;
  action(win, "win.close_tab");
  wait_history(app_window_get_active_tab(win));
  g_assert_true(thread_store_reserve_delete(id));
  g_assert_false(thread_store_reserve_delete(id));
  g_assert_null(tab_load(win, id));
  thread_store_release_delete(id);
  action(win, "win.history_list");
  g_autoptr(GtkWidget) history = find_window("Saved conversations");
  g_assert_nonnull(history);
  GtkWidget *remove = NULL;
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while ((remove = find_button(history, "Delete")) == NULL &&
         g_get_monotonic_time() < deadline)
    settle(50);
  g_assert_nonnull(remove);
  action(win, "win.restore_tab");
  tab = app_window_get_active_tab(win);
  g_assert_cmpstr(tab->id, ==, id);
  wait_history(tab);
  g_signal_emit_by_name(remove, "clicked");
  settle(100);
  GtkWidget *confirm = NULL;
  GListModel *windows = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    g_autoptr(GtkWindow) candidate = g_list_model_get_item(windows, i);
    confirm = find_button(GTK_WIDGET(candidate), "Delete conversation");
    if (confirm != NULL)
      break;
  }
  g_assert_nonnull(confirm);
  g_signal_emit_by_name(confirm, "clicked");
  settle(100);
  g_autoptr(JsonArray) messages = thread_store_messages(tab->store, NULL);
  g_assert_nonnull(messages);
  g_assert_cmpuint(json_array_get_length(messages), ==, 240);
  gtk_window_destroy(GTK_WINDOW(history));
  g_print("Audit regressions passed: exhausted paging, restore reservation, "
          "and stale-row deletion.\n");
}

static void test_last_tab_close(AppWindow *win) {
  GLogLevelFlags previous =
      g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  g_assert_cmpuint(win->tabs->len, ==, 1);
  for (guint i = 0; i < 6; i++) {
    Tab *closing = app_window_get_active_tab(win);
    wait_history(closing);
    prompt(closing, "Last-tab close regression");
    g_autofree char *old_id = g_strdup(closing->id);
    if (i % 2 == 0) {
      action(win, "win.popout");
      g_assert_true(closing->output_popped);
    }
    action(win, "win.close_tab");
    g_assert_cmpuint(win->tabs->len, ==, 1);
    Tab *replacement = app_window_get_active_tab(win);
    g_assert_cmpstr(replacement->id, !=, old_id);
    g_assert_true(replacement->hydrated);
    g_assert_true(GTK_IS_TEXT_VIEW(replacement->prompt_view));
    g_assert_true(GTK_IS_TEXT_VIEW(replacement->output_view));
    g_assert_true(GTK_IS_BUTTON(replacement->copy_btn));
    wait_history(replacement);
    action(win, "win.popout");
    g_assert_true(replacement->output_popped);
    g_assert_true(GTK_IS_WINDOW(replacement->popout_window));
    gtk_window_close(GTK_WINDOW(replacement->popout_window));
    g_assert_false(replacement->output_popped);
    g_assert_null(replacement->popout_window);
    g_assert_true(GTK_IS_TEXT_VIEW(replacement->output_view));
    if (i == 0) {
      action(win, "win.restore_tab");
      Tab *restored = app_window_get_active_tab(win);
      g_assert_cmpstr(restored->id, ==, old_id);
      wait_history(restored);
      GtkTextBuffer *buffer =
          gtk_text_view_get_buffer(GTK_TEXT_VIEW(restored->prompt_view));
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds(buffer, &start, &end);
      g_autofree char *draft =
          gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
      g_assert_cmpstr(draft, ==, "Last-tab close regression");
      action(win, "win.popout");
      g_assert_true(restored->output_popped);
      action(win, "win.close_tab");
      g_assert_cmpuint(win->tabs->len, ==, 1);
      g_assert_true(app_window_get_active_tab(win) == replacement);
      wait_history(replacement);
    }
  }
  Tab *background = app_window_get_active_tab(win);
  action(win, "win.popout");
  action(win, "win.new_tab");
  g_assert_false(background->output_popped);
  g_assert_false(background->hydrated);
  wait_history(app_window_get_active_tab(win));
  action(win, "win.close_tab");
  g_assert_true(app_window_get_active_tab(win) == background);
  wait_history(background);
  g_assert_true(background->hydrated);
  g_assert_cmpuint(win->tabs->len, ==, 1);
  g_log_set_always_fatal(previous);
  g_print("Last-tab closure, replacement editors, and popout lifetime checks "
          "passed.\n");
}

static void test_tab_rename(AppWindow *win, const char *artifact_dir) {
  Tab *tab = app_window_get_active_tab(win);
  GtkWidget *header =
      gtk_notebook_get_tab_label(GTK_NOTEBOOK(win->tab_bar), tab->page);
  settle(100);
  g_assert_false(pango_layout_is_ellipsized(
      gtk_label_get_layout(GTK_LABEL(tab->tab_name_label))));
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(header);
  g_autoptr(GtkEventController) click = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) candidate =
        g_list_model_get_item(controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(candidate),
                  "promptr-tab-title") == 0)
      click = g_object_ref(candidate);
  }
  g_assert_nonnull(click);
  emit_double_click(GTK_GESTURE_CLICK(click), 5, 5);
  g_assert_true(GTK_IS_ENTRY(tab->rename_entry));
  gtk_editable_set_text(GTK_EDITABLE(tab->rename_entry),
                        "Renamed conversation");
  settle(100);
  g_autofree char *path =
      g_build_filename(artifact_dir, "tab-rename.png", NULL);
  screenshot(win->window, path);
  g_signal_emit_by_name(tab->rename_entry, "activate");
  g_assert_null(tab->rename_entry);
  g_assert_cmpstr(tab->name, ==, "Renamed conversation");
  settle(100);
  g_assert_false(pango_layout_is_ellipsized(
      gtk_label_get_layout(GTK_LABEL(tab->tab_name_label))));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tab->tab_name_label)), ==,
                  tab->name);
  emit_double_click(GTK_GESTURE_CLICK(click), 5, 5);
  g_assert_true(GTK_IS_ENTRY(tab->rename_entry));
  gtk_editable_set_text(GTK_EDITABLE(tab->rename_entry), "Discard this name");
  g_autoptr(GListModel) keys =
      gtk_widget_observe_controllers(tab->rename_entry);
  gboolean cancelled = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(keys); i++) {
    g_autoptr(GtkEventController) key = g_list_model_get_item(keys, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(key)) {
      g_signal_emit_by_name(key, "key-pressed", GDK_KEY_Escape, 0, 0,
                            &cancelled);
      break;
    }
  }
  g_assert_true(cancelled);
  g_assert_null(tab->rename_entry);
  g_assert_cmpstr(tab->name, ==, "Renamed conversation");
}

static void test_tab_strip(AppWindow *win, const char *artifact_dir) {
  g_autoptr(GListModel) controllers =
      gtk_widget_observe_controllers(win->tab_bar);
  g_autoptr(GtkEventController) click = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) candidate =
        g_list_model_get_item(controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(candidate),
                  "promptr-tab-strip") == 0)
      click = g_object_ref(candidate);
  }
  g_assert_nonnull(click);
  GtkPositionType positions[] = {GTK_POS_TOP, GTK_POS_BOTTOM};
  for (guint p = 0; p < G_N_ELEMENTS(positions); p++) {
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(win->tab_bar), positions[p]);
    settle(100);
    Tab *tab = app_window_get_active_tab(win);
    wait_history(tab);
    guint count = win->tabs->len;
    GtkWidget *editors[] = {tab->prompt_view, tab->output_view};
    for (guint i = 0; i < G_N_ELEMENTS(editors); i++) {
      graphene_rect_t bounds;
      g_assert_true(
          gtk_widget_compute_bounds(editors[i], win->tab_bar, &bounds));
      emit_double_click(GTK_GESTURE_CLICK(click),
                        bounds.origin.x + bounds.size.width * 0.8,
                        bounds.origin.y + bounds.size.height / 2);
      g_assert_cmpuint(win->tabs->len, ==, count);
    }
    GtkWidget *label =
        gtk_notebook_get_tab_label(GTK_NOTEBOOK(win->tab_bar), tab->page);
    graphene_rect_t occupied;
    g_assert_true(gtk_widget_compute_bounds(label, win->tab_bar, &occupied));
    emit_double_click(GTK_GESTURE_CLICK(click),
                      occupied.origin.x + occupied.size.width / 2,
                      occupied.origin.y + occupied.size.height / 2);
    g_assert_cmpuint(win->tabs->len, ==, count);
    GtkWidget *header = gtk_widget_get_first_child(win->tab_bar);
    while (header != NULL &&
           g_strcmp0(gtk_widget_get_css_name(header), "header") != 0)
      header = gtk_widget_get_next_sibling(header);
    g_assert_nonnull(header);
    graphene_rect_t empty;
    g_assert_true(gtk_widget_compute_bounds(header, win->tab_bar, &empty));
    emit_double_click(GTK_GESTURE_CLICK(click),
                      empty.origin.x + empty.size.width - 8,
                      empty.origin.y + empty.size.height / 2);
    g_assert_cmpuint(win->tabs->len, ==, count + 1);
    wait_history(app_window_get_active_tab(win));
    if (positions[p] == GTK_POS_BOTTOM) {
      g_autofree char *path =
          g_build_filename(artifact_dir, "tabs-bottom.png", NULL);
      screenshot(win->window, path);
    }
    action(win, "win.close_tab");
    g_assert_cmpuint(win->tabs->len, ==, count);
    wait_history(app_window_get_active_tab(win));
  }
  gtk_notebook_set_tab_pos(GTK_NOTEBOOK(win->tab_bar), GTK_POS_TOP);
  settle(100);
}

static void set_font_button(GtkWidget *button, const char *name) {
  g_autoptr(PangoFontDescription) description =
      pango_font_description_from_string(name);
  gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button),
                                       description);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static char *choose_font(GtkWidget *button, const char *title,
                         const char *name) {
  g_assert_true(gtk_widget_activate(button));
  settle(200);
  g_autoptr(GtkWidget) chooser = find_window(title);
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (chooser == NULL && g_get_monotonic_time() < deadline) {
    settle(100);
    chooser = find_window(title);
  }
  if (chooser == NULL) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
      g_autoptr(GtkWindow) window = g_list_model_get_item(windows, i);
      g_print("Font chooser diagnostic: %s %s visible=%d\n",
              G_OBJECT_TYPE_NAME(window), gtk_window_get_title(window),
              gtk_widget_get_visible(GTK_WIDGET(window)));
    }
  }
  g_assert_nonnull(chooser);
  g_assert_true(GTK_IS_FONT_CHOOSER(chooser));
  g_autoptr(PangoFontDescription) requested =
      pango_font_description_from_string(name);
  g_autoptr(PangoFont) resolved =
      pango_context_load_font(gtk_widget_get_pango_context(button), requested);
  g_assert_nonnull(resolved);
  g_autoptr(PangoFontDescription) actual = pango_font_describe(resolved);
  gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(chooser), actual);
  settle(150);
  char *selected = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(chooser));
  deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (selected == NULL && g_get_monotonic_time() < deadline) {
    settle(200);
    gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(chooser), actual);
    selected = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(chooser));
  }
  g_assert_nonnull(selected);
  gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_OK);
  settle(200);
  return selected;
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void assert_rendered_spacing(GtkWidget *view, gboolean proportional) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *original =
      gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  gtk_text_buffer_set_text(buffer, "iiiiWWWW", -1);
  settle(100);
  GtkTextIter middle;
  gtk_text_buffer_get_start_iter(buffer, &start);
  gtk_text_buffer_get_iter_at_offset(buffer, &middle, 4);
  gtk_text_buffer_get_end_iter(buffer, &end);
  GdkRectangle first, second, last;
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &start, &first);
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &middle, &second);
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &end, &last);
  int narrow_width = second.x - first.x;
  int wide_width = last.x - second.x;
  g_assert_cmpint(narrow_width, >, 0);
  if (proportional)
    g_assert_cmpint(wide_width, >, narrow_width * 3 / 2);
  else
    g_assert_cmpint(ABS(wide_width - narrow_width), <=, 1);
  gtk_text_buffer_set_text(buffer, original, -1);
}

static void test_preferences_sections(AppWindow *win,
                                      const char *artifact_dir) {
  /* GTK user CSS outranks application CSS. Match the user's installed theme. */
  g_autoptr(GtkCssProvider) user_theme = gtk_css_provider_new();
  gtk_css_provider_load_from_string(user_theme,
                                    ".monospace { font-family: Monospace; }");
  GdkDisplay *display = gtk_widget_get_display(win->window);
  gtk_style_context_add_provider_for_display(display,
                                             GTK_STYLE_PROVIDER(user_theme),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) settings = find_window("Preferences");
  g_assert_nonnull(settings);
  GtkWidget *sections = find_type(settings, GTK_TYPE_NOTEBOOK);
  g_assert_true(GTK_IS_NOTEBOOK(sections));
  g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(sections)), ==, 3);
  g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(sections)), ==, 0);
  settle(100);
  GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(sections), 0);
  GtkWidget *grid = gtk_widget_get_first_child(page);
  GtkWidget *query_font = gtk_grid_get_child_at(GTK_GRID(grid), 1, 0);
  GtkWidget *answer_font = gtk_grid_get_child_at(GTK_GRID(grid), 1, 1);
  GtkWidget *numbers = gtk_grid_get_child_at(GTK_GRID(grid), 1, 2);
  g_assert_true(GTK_IS_FONT_DIALOG_BUTTON(query_font));
  g_autofree char *initial_query =
      runtime_config_get_string(win->config, "query_font", QUERY_FONT_DEFAULT);
  g_autofree char *initial_answer = runtime_config_get_string(
      win->config, "answer_font", ANSWER_FONT_DEFAULT);
  g_autoptr(PangoFontDescription) initial_query_desc =
      pango_font_description_from_string(initial_query);
  g_autoptr(PangoFontDescription) initial_answer_desc =
      pango_font_description_from_string(initial_answer);
  g_assert_cmpstr(
      pango_font_description_get_family(gtk_font_dialog_button_get_font_desc(
          GTK_FONT_DIALOG_BUTTON(query_font))),
      ==, pango_font_description_get_family(initial_query_desc));
  g_assert_cmpstr(
      pango_font_description_get_family(gtk_font_dialog_button_get_font_desc(
          GTK_FONT_DIALOG_BUTTON(answer_font))),
      ==, pango_font_description_get_family(initial_answer_desc));

  GtkWidget *apply = find_button(settings, "Apply");
  g_assert_false(gtk_widget_get_sensitive(apply));
  g_autofree char *chosen_query =
      choose_font(query_font, "Query font", "Serif Bold 14");
  g_autofree char *chosen_answer =
      choose_font(answer_font, "Answer font", "Monospace 12");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(numbers), FALSE);
  g_assert_true(gtk_widget_get_sensitive(apply));
  g_signal_emit_by_name(apply, "clicked");
  g_assert_false(gtk_widget_get_sensitive(apply));
  Tab *tab = app_window_get_active_tab(win);
  settle(100);
  g_autoptr(PangoFontDescription) expected_query =
      pango_font_description_from_string(chosen_query);
  g_autoptr(PangoFontDescription) expected_answer =
      pango_font_description_from_string(chosen_answer);
  const PangoFontDescription *rendered_font =
      pango_context_get_font_description(
          gtk_widget_get_pango_context(tab->prompt_view));
  g_assert_cmpstr(pango_font_description_get_family(rendered_font), ==,
                  pango_font_description_get_family(expected_query));
  double query_points =
      (double)pango_font_description_get_size(rendered_font) / PANGO_SCALE;
  if (pango_font_description_get_size_is_absolute(rendered_font))
    query_points *= 72.0 / pango_cairo_context_get_resolution(
                               gtk_widget_get_pango_context(tab->prompt_view));
  g_assert_cmpfloat_with_epsilon(query_points, 14.0, 0.01);
  g_assert_cmpint(pango_font_description_get_weight(rendered_font), ==,
                  PANGO_WEIGHT_BOLD);
  rendered_font = pango_context_get_font_description(
      gtk_widget_get_pango_context(tab->output_view));
  g_assert_cmpstr(pango_font_description_get_family(rendered_font), ==,
                  pango_font_description_get_family(expected_answer));
  double answer_points =
      (double)pango_font_description_get_size(rendered_font) / PANGO_SCALE;
  if (pango_font_description_get_size_is_absolute(rendered_font))
    answer_points *= 72.0 / pango_cairo_context_get_resolution(
                                gtk_widget_get_pango_context(tab->output_view));
  g_assert_cmpfloat_with_epsilon(answer_points, 12.0, 0.01);
  g_assert_false(gtk_text_view_get_monospace(GTK_TEXT_VIEW(tab->prompt_view)));
  g_assert_false(gtk_text_view_get_monospace(GTK_TEXT_VIEW(tab->output_view)));
  assert_rendered_spacing(tab->prompt_view, TRUE);
  assert_rendered_spacing(tab->output_view, FALSE);

  g_assert_false(
      gtk_source_view_get_show_line_numbers(GTK_SOURCE_VIEW(tab->prompt_view)));
  g_assert_false(
      gtk_source_view_get_show_line_numbers(GTK_SOURCE_VIEW(tab->output_view)));
  action(win, "win.new_tab");
  Tab *new_tab = app_window_get_active_tab(win);
  wait_history(new_tab);
  g_assert_false(gtk_source_view_get_show_line_numbers(
      GTK_SOURCE_VIEW(new_tab->prompt_view)));
  action(win, "win.close_tab");
  wait_history(app_window_get_active_tab(win));
  g_autofree char *query_config =
      runtime_config_get_string(win->config, "query_font", "");
  g_assert_cmpstr(query_config, ==, chosen_query);
  RuntimeConfig *saved = runtime_config_load();
  g_autofree char *answer_config =
      runtime_config_get_string(saved, "answer_font", "");
  g_assert_cmpstr(answer_config, ==, chosen_answer);
  g_assert_false(runtime_config_get_bool(saved, "editor_line_numbers", TRUE));
  runtime_config_free(saved);
  g_autofree char *editor_path =
      g_build_filename(artifact_dir, "preferences-editor.png", NULL);
  screenshot(settings, editor_path);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 1);
  g_autofree char *keys_path =
      g_build_filename(artifact_dir, "preferences-shortcuts.png", NULL);
  screenshot(settings, keys_path);
  GtkWidget *bindings = find_list(settings);
  g_assert_nonnull(bindings);
  gtk_list_box_select_row(
      GTK_LIST_BOX(bindings),
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(bindings), 0));
  GtkWidget *change = find_button(settings, "Change");
  g_assert_true(gtk_widget_get_sensitive(change));
  g_signal_emit_by_name(change, "clicked");
  settle(100);
  g_autoptr(GtkWidget) capture = find_window("Change shortcut");
  g_assert_nonnull(capture);
  GtkWidget *capture_entry = find_type(capture, GTK_TYPE_ENTRY);
  g_assert_nonnull(capture_entry);
  g_autoptr(GListModel) capture_controllers =
      gtk_widget_observe_controllers(capture_entry);
  gboolean captured = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(capture_controllers); i++) {
    g_autoptr(GtkEventController) controller =
        g_list_model_get_item(capture_controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller),
                  "promptr-shortcut-capture") == 0) {
      g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_k, 0,
                            GDK_CONTROL_MASK | GDK_ALT_MASK, &captured);
      break;
    }
  }
  g_assert_true(captured);
  guint captured_key = 0;
  GdkModifierType captured_mods = 0;
  g_assert_true(
      gtk_accelerator_parse(gtk_editable_get_text(GTK_EDITABLE(capture_entry)),
                            &captured_key, &captured_mods));
  g_assert_cmpuint(captured_key, ==, GDK_KEY_k);
  g_assert_cmpuint(captured_mods, ==, GDK_CONTROL_MASK | GDK_ALT_MASK);

  g_signal_emit_by_name(find_button(capture, "OK"), "clicked");
  g_assert_true(gtk_widget_get_sensitive(apply));
  g_signal_emit_by_name(apply, "clicked");
  g_assert_cmpuint(win->kb_focus_mods, ==, GDK_CONTROL_MASK | GDK_ALT_MASK);
  g_assert_cmpuint(win->kb_focus_keyval, ==, GDK_KEY_k);
  g_signal_emit_by_name(change, "clicked");
  settle(100);
  g_autoptr(GtkWidget) conflict_capture = find_window("Change shortcut");
  capture_entry = find_type(conflict_capture, GTK_TYPE_ENTRY);
  gtk_editable_set_text(GTK_EDITABLE(capture_entry), "<Control>Return");
  g_signal_emit_by_name(find_button(conflict_capture, "OK"), "clicked");
  g_autoptr(GtkWidget) conflict = find_window("Shortcut already in use");
  g_assert_nonnull(conflict);
  g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(conflict)) ==
                GTK_WINDOW(conflict_capture));
  gtk_window_destroy(GTK_WINDOW(conflict));
  gtk_editable_set_text(GTK_EDITABLE(capture_entry), KB_FOCUS_PROMPT);
  g_signal_emit_by_name(find_button(conflict_capture, "OK"), "clicked");
  g_signal_emit_by_name(apply, "clicked");
  g_assert_cmpuint(win->kb_focus_mods, ==, GDK_CONTROL_MASK);

  /* Errors raised from the main window must belong to its open modal. */
  g_assert_true(app_window_dialog_parent(GTK_WINDOW(win->window)) ==
                GTK_WINDOW(settings));
  show_error_dialog(GTK_WINDOW(win->window), "Fixture confirmation",
                    "Dialog stacking check");
  g_autoptr(GtkWidget) confirmation = find_window("Fixture confirmation");
  g_assert_nonnull(confirmation);
  g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(confirmation)) ==
                GTK_WINDOW(settings));
  g_assert_true(gtk_window_get_modal(GTK_WINDOW(confirmation)));
  g_assert_true(app_window_dialog_parent(GTK_WINDOW(win->window)) ==
                GTK_WINDOW(confirmation));
  app_window_present(win);
  g_autofree char *dialog_path =
      g_build_filename(artifact_dir, "preferences-confirmation.png", NULL);
  screenshot(confirmation, dialog_path);
  gtk_window_destroy(GTK_WINDOW(confirmation));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 0);
  set_font_button(query_font, "Serif 15");
  gtk_window_close(GTK_WINDOW(settings));
  settle(200);
  GListModel *windows = gtk_window_get_toplevels();
  g_autoptr(GtkWindow) discard_dialog = NULL;
  GtkWidget *keep = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    g_autoptr(GtkWindow) candidate = g_list_model_get_item(windows, i);
    keep = find_button(GTK_WIDGET(candidate), "Keep editing");
    if (keep != NULL) {
      discard_dialog = g_object_ref(candidate);
      break;
    }
  }
  g_assert_nonnull(discard_dialog);
  g_assert_true(gtk_window_get_transient_for(discard_dialog) ==
                GTK_WINDOW(settings));
  g_assert_true(gtk_window_get_modal(discard_dialog));
  g_autofree char *discard_path =
      g_build_filename(artifact_dir, "preferences-discard.png", NULL);
  screenshot(GTK_WIDGET(discard_dialog), discard_path);
  g_signal_emit_by_name(keep, "clicked");
  settle(100);
  g_assert_true(gtk_widget_get_visible(settings));
  set_font_button(query_font, "Monospace 11");
  set_font_button(answer_font, "Monospace 11");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(numbers), TRUE);
  g_signal_emit_by_name(apply, "clicked");
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 1);
  settle(100);
  GtkListBoxRow *first_binding =
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(bindings), 0);
  g_autoptr(GListModel) inline_controllers =
      gtk_widget_observe_controllers(GTK_WIDGET(first_binding));
  for (guint i = 0; i < g_list_model_get_n_items(inline_controllers); i++) {
    g_autoptr(GtkEventController) controller =
        g_list_model_get_item(inline_controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller),
                  "promptr-shortcut-inline") == 0) {
      emit_double_click(GTK_GESTURE_CLICK(controller), 10, 5);
      break;
    }
  }
  GtkWidget *inline_entry =
      find_type(GTK_WIDGET(first_binding), GTK_TYPE_ENTRY);
  g_assert_nonnull(inline_entry);
  gtk_editable_set_text(GTK_EDITABLE(inline_entry), "<Control><Shift>u");
  /* Apply must finish an inline edit even without pressing enter first. */
  g_signal_emit_by_name(apply, "clicked");
  g_assert_cmpuint(win->kb_focus_keyval, ==, GDK_KEY_u);
  g_assert_cmpuint(win->kb_focus_mods, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 0);
  set_font_button(query_font, "Serif 18");
  g_signal_emit_by_name(find_button(settings, "Cancel"), "clicked");
  g_autofree char *cancelled_font =
      runtime_config_get_string(win->config, "query_font", "");
  g_assert_cmpstr(cancelled_font, ==, "Monospace 11");
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) reopened = find_window("Preferences");
  GtkWidget *reopened_sections = find_type(reopened, GTK_TYPE_NOTEBOOK);
  GtkWidget *reopened_page =
      gtk_notebook_get_nth_page(GTK_NOTEBOOK(reopened_sections), 0);
  GtkWidget *reopened_grid = gtk_widget_get_first_child(reopened_page);
  GtkWidget *reopened_size =
      gtk_grid_get_child_at(GTK_GRID(reopened_grid), 1, 0);
  const PangoFontDescription *reopened_font =
      gtk_font_dialog_button_get_font_desc(
          GTK_FONT_DIALOG_BUTTON(reopened_size));
  g_assert_cmpstr(pango_font_description_get_family(reopened_font), ==,
                  "Monospace");
  g_assert_cmpint(pango_font_description_get_size(reopened_font), ==,
                  11 * PANGO_SCALE);
  set_font_button(reopened_size, "Monospace 13");
  g_signal_emit_by_name(find_button(reopened, "OK"), "clicked");
  g_autofree char *ok_font =
      runtime_config_get_string(win->config, "query_font", "");
  g_assert_cmpstr(ok_font, ==, "Monospace 13");
  g_assert_false(gtk_widget_get_visible(reopened));
  gtk_style_context_remove_provider_for_display(display,
                                                GTK_STYLE_PROVIDER(user_theme));
}

static char *add_profile(AppWindow *win, const char *name, const char *mode,
                         const char *artifact_dir) {
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) settings = find_window("Preferences");
  g_assert_nonnull(settings);
  GtkWidget *sections = find_type(settings, GTK_TYPE_NOTEBOOK);
  g_assert_true(GTK_IS_NOTEBOOK(sections));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 2);
  settle(100);
  g_assert_true(gtk_window_get_modal(GTK_WINDOW(settings)));
  if (g_strcmp0(mode, "normal") == 0) {
    g_autofree char *path =
        g_build_filename(artifact_dir, "preferences-default.png", NULL);
    screenshot(settings, path);
  }
  GtkWidget *add = find_button(settings, "Add provider");
  g_assert_nonnull(add);
  g_signal_emit_by_name(add, "clicked");
  guint position;
  GtkWidget *plugins = find_choice(settings, "custom", &position);
  g_assert_nonnull(plugins);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(plugins), position);
  GtkWidget *name_entry = find_entry(settings, "Profile name"),
            *command = find_entry(settings, "Plugin executable");
  GtkWidget *arguments = find_entry(settings, "Arguments");
  g_assert_nonnull(name_entry);
  g_assert_nonnull(command);
  g_assert_nonnull(arguments);
  gtk_editable_set_text(GTK_EDITABLE(name_entry), name);
  g_autofree char *python = g_find_program_in_path("python3");
  gtk_editable_set_text(GTK_EDITABLE(command), python);
  g_autofree char *quoted = g_shell_quote(g_getenv("PROMPTR_TEST_FIXTURE"));
  g_autofree char *args = g_strdup_printf("%s %s", quoted, mode);
  gtk_editable_set_text(GTK_EDITABLE(arguments), args);
  GtkWidget *save = find_button(settings, "Apply"),
            *test = find_button(settings, "Test connection");
  g_assert_nonnull(save);
  g_assert_nonnull(test);
  g_assert_false(gtk_widget_get_sensitive(save));
  g_signal_emit_by_name(test, "clicked");
  gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (!gtk_widget_get_sensitive(save)) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  /* Display-only renaming does not invalidate backend connectivity. */
  gtk_editable_set_text(GTK_EDITABLE(name_entry), name);
  g_assert_true(gtk_widget_get_sensitive(save));
  if (g_strcmp0(mode, "normal") == 0) {
    g_autofree char *path =
        g_build_filename(artifact_dir, "providers.png", NULL);
    screenshot(settings, path);
  }
  g_signal_emit_by_name(save, "clicked");
  char *id = NULL;
  while (id == NULL) {
    pump();
    g_autoptr(GPtrArray) profiles = provider_profile_list();
    for (guint i = 0; i < profiles->len; i++) {
      ProviderProfile *profile = g_ptr_array_index(profiles, i);
      if (g_strcmp0(profile->name, name) == 0)
        id = g_strdup(profile->id);
    }
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  settle(50);
  gtk_editable_set_text(GTK_EDITABLE(arguments), "changed settings");
  g_assert_false(gtk_widget_get_sensitive(save));
  GtkWidget *profiles = find_list(settings);
  g_assert_nonnull(profiles);
  gtk_list_box_select_row(
      GTK_LIST_BOX(profiles),
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(profiles), 0));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(name_entry)), ==, name);
  GtkWidget *discard = find_button(settings, "Discard changes");
  g_assert_nonnull(discard);
  g_signal_emit_by_name(discard, "clicked");
  g_assert_false(gtk_widget_get_visible(discard));
  gtk_window_set_focus(GTK_WINDOW(settings), NULL);
  gtk_window_destroy(GTK_WINDOW(settings));
  return id;
}

static void test_connection_cancel(AppWindow *win, const char *artifact_dir) {
  action(win, "win.manage_providers");
  g_autoptr(GtkWidget) settings = find_window("Preferences");
  g_assert_nonnull(settings);
  GtkWidget *sections = find_type(settings, GTK_TYPE_NOTEBOOK);
  g_assert_true(GTK_IS_NOTEBOOK(sections));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(sections), 2);
  settle(100);
  g_signal_emit_by_name(find_button(settings, "Add provider"), "clicked");
  guint position;
  GtkWidget *plugins = find_choice(settings, "custom", &position);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(plugins), position);
  g_autofree char *python = g_find_program_in_path("python3");
  gtk_editable_set_text(GTK_EDITABLE(find_entry(settings, "Plugin executable")),
                        python);
  g_autofree char *quoted = g_shell_quote(g_getenv("PROMPTR_TEST_FIXTURE"));
  g_autofree char *args = g_strdup_printf("%s stall_reply", quoted);
  gtk_editable_set_text(GTK_EDITABLE(find_entry(settings, "Arguments")), args);
  GtkWidget *test = find_button(settings, "Test connection");
  g_signal_emit_by_name(test, "clicked");
  settle(2200);
  GtkWidget *cancel = find_button(settings, "Stop test");
  g_assert_nonnull(cancel);
  g_assert_true(gtk_widget_get_visible(cancel));
  g_autofree char *path =
      g_build_filename(artifact_dir, "preferences-testing.png", NULL);
  screenshot(settings, path);
  g_signal_emit_by_name(cancel, "clicked");
  gint64 until = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
  while (!gtk_widget_get_sensitive(test)) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, until);
  }
  g_assert_false(gtk_widget_get_visible(cancel));
  g_assert_false(gtk_widget_get_sensitive(find_button(settings, "Apply")));
  gtk_window_set_focus(GTK_WINDOW(settings), NULL);
  gtk_window_destroy(GTK_WINDOW(settings));
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  signal(SIGPIPE, SIG_IGN);
  app_diagnostics_install(gtk_get_major_version(), gtk_get_minor_version(),
                          gtk_get_micro_version());
  gtk_init();
  gtk_source_init();
  g_autoptr(GtkApplication) app = gtk_application_new(
      "com.toxdes.promptr.ui-test", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
  gint64 started = g_get_monotonic_time();
  AppWindow *win = app_window_new(app);
  app_window_show(win);
  wait_history(app_window_get_active_tab(win));
  settle(100);
  gint64 startup = g_get_monotonic_time() - started;
  guint64 baseline = rss();
  Tab *initial = app_window_get_active_tab(win);
  g_autoptr(ProviderProfile) preset =
      provider_profile_load(initial->provider_name, NULL);
  g_assert_nonnull(preset);
  g_assert_cmpstr(preset->plugin, ==, "openrouter");
  g_assert_null(find_button(win->window, "Providers"));
  g_assert_null(find_button(win->window, "Choose project"));
  g_assert_null(find_button(win->window, "Model ID"));
  gint64 models_deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (win->model_pending) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, models_deadline);
  }
  if (g_strcmp0(g_getenv("PROMPTR_UI_SCENARIO"), "audit") == 0) {
    test_audit_regressions(win);
    app_window_close_and_quit(win);
    app_window_free(win);
    settle(100);
    return 0;
  }
  if (g_strcmp0(g_getenv("PROMPTR_UI_SCENARIO"), "model-selection") == 0) {
    test_model_selection(win);
    test_connection_error_details(win, argv[1]);
    app_window_close_and_quit(win);
    app_window_free(win);
    settle(100);
    return 0;
  }
  if (g_strcmp0(g_getenv("PROMPTR_UI_SCENARIO"), "last-tab") == 0) {
    test_last_tab_close(win);
    app_window_close_and_quit(win);
    app_window_free(win);
    settle(100);
    return 0;
  }
  guint model_position;
  g_assert_nonnull(find_choice(win->window, "fixture/first", &model_position));
  GtkWidget *file_item = gtk_widget_get_first_child(win->menu_bar);
  GtkWidget *edit_item =
      file_item != NULL ? gtk_widget_get_next_sibling(file_item) : NULL;
  if (edit_item != NULL && gtk_widget_mnemonic_activate(edit_item, FALSE)) {
    settle(150);
    GtkWidget *popover = find_open_popover(win->menu_bar);
    if (popover != NULL) {
      g_autofree char *path = g_build_filename(argv[1], "menu-edit.png", NULL);
      screenshot(popover, path);
      gtk_popover_popdown(GTK_POPOVER(popover));
      settle(50);
    }
  }
  g_assert_false(gtk_widget_get_sensitive(initial->agent_dropdown));
  test_model_search(win, argv[1]);
  test_last_tab_close(win);
  test_tab_rename(win, argv[1]);
  test_tab_strip(win, argv[1]);
  test_preferences_sections(win, argv[1]);
  g_autofree char *first = add_profile(win, "Offline", "normal", argv[1]);
  g_autofree char *streaming = add_profile(win, "Streaming", "stream", argv[1]);
  test_connection_cancel(win, argv[1]);
  g_autofree char *no_models =
      add_profile(win, "No models", "no_models", argv[1]);
  g_assert_true(gtk_widget_activate_action(win->window, "win.set_provider", "s",
                                           no_models));
  gint64 model_wait = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (win->model_pending) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, model_wait);
  }
  Tab *empty_model = app_window_get_active_tab(win);
  g_assert_false(gtk_widget_get_sensitive(empty_model->model_dropdown));
  g_assert_false(gtk_widget_get_sensitive(empty_model->agent_dropdown));
  prompt(empty_model, "no-model question");
  action(win, "win.submit");
  while (empty_model->state == STATE_LOADING) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, model_wait);
  }
  wait_history(empty_model);
  g_assert_false(gtk_widget_get_sensitive(empty_model->model_dropdown));
  g_assert_false(gtk_widget_get_sensitive(empty_model->agent_dropdown));
  Tab *a = app_window_get_active_tab(win);
  g_assert_true(gtk_widget_activate_action(win->window, "win.set_provider", "s",
                                           streaming));
  prompt(a, "first question");
  action(win, "win.submit");
  g_assert_cmpint(a->state, ==, STATE_LOADING);
  settle(80);
  action(win, "win.new_tab");
  Tab *b = app_window_get_active_tab(win);
  g_assert_true(a != b);
  g_assert_false(a->hydrated);
  g_assert_null(a->output_view);
  g_assert_nonnull(a->provider);
  g_assert_true(gtk_widget_activate_action(win->window, "win.set_provider", "s",
                                           streaming));
  prompt(b, "second question");
  action(win, "win.submit");
  settle(50);
  guint64 two_pending = rss();
  action(win, "win.cancel");
  gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (a->state == STATE_LOADING || b->state == STATE_LOADING) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  g_assert_cmpint(a->state, ==, STATE_FINISHED);
  g_assert_cmpint(b->state, ==, STATE_CANCELED);
  g_assert_null(a->provider);
  g_assert_null(a->store);
  g_assert_true(a->has_unseen_output);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), 0);
  wait_history(a);
  g_autofree char *answer = output(a);
  g_assert_nonnull(strstr(answer, "partial"));
  g_assert_true(
      gtk_widget_activate_action(win->window, "win.set_provider", "s", first));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(a->follow_up_check), TRUE);
  prompt(a, "follow-up question");
  action(win, "win.submit");
  deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  while (a->state == STATE_LOADING) {
    pump();
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  wait_history(a);
  g_autofree char *follow_up = output(a);
  g_assert_true(g_str_has_prefix(follow_up,
                                 "pong\n---\nQ.1: first question\nA: partial"));
  g_assert_null(strstr(follow_up, "follow-up question"));
  /* A draft and a custom gutter mark survive eviction and hydration. */
  prompt(a, "saved draft");
  GtkTextBuffer *buffer =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->output_view));
  GtkTextIter line;
  gtk_text_buffer_get_iter_at_line(buffer, &line, 0);
  gtk_source_buffer_create_source_mark(GTK_SOURCE_BUFFER(buffer), NULL,
                                       "promptr-mark", &line);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), 1);
  wait_history(b);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), 0);
  wait_history(a);
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->prompt_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *draft =
      gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(draft, ==, "saved draft");
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->output_view));
  GSList *marks = gtk_source_buffer_get_source_marks_at_line(
      GTK_SOURCE_BUFFER(buffer), 0, "promptr-mark");
  g_assert_nonnull(marks);
  g_slist_free(marks);
  g_autofree char *main_image =
      g_build_filename(argv[1], "conversation.png", NULL);
  screenshot(win->window, main_image);
  /* Close, restore, then keep many idle tabs without materializing their views.
   */
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), 1);
  wait_history(b);
  g_autofree char *closed_id = g_strdup(b->id);
  action(win, "win.close_tab");
  action(win, "win.restore_tab");
  g_assert_cmpstr(app_window_get_active_tab(win)->id, ==, closed_id);
  for (guint i = 0; i < 100; i++)
    action(win, "win.new_tab");
  wait_history(app_window_get_active_tab(win));
  settle(200);
  guint views = 0, providers = 0;
  for (guint i = 0; i < win->tabs->len; i++) {
    Tab *tab = g_ptr_array_index(win->tabs, i);
    views += tab->hydrated;
    providers += tab->provider != NULL;
  }
  g_assert_cmpuint(views, ==, 1);
  g_assert_cmpuint(providers, ==, 0);
  guint64 idle = rss();
  g_print("{\"startup_us\":%" G_GINT64_FORMAT
          ",\"baseline_gui_kib\":%" G_GUINT64_FORMAT
          ",\"two_pending_gui_kib\":%" G_GUINT64_FORMAT
          ",\"many_idle_gui_kib\":%" G_GUINT64_FORMAT
          ",\"tabs\":%u,\"views\":%u}\n",
          startup, baseline, two_pending, idle, win->tabs->len, views);
  app_window_close_and_quit(win);
  app_window_free(win);
  settle(100);
  return 0;
}
