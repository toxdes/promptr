#include "provider-settings.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "config.h"
#include "plugin-manager.h"
#include "plugin-runner.h"
#include "provider-manager.h"
#include "provider-profile.h"
#include "shortcuts.h"

typedef struct _Editor Editor;
typedef struct {
  Editor *editor;
  const char *group, *label, *key, *fallback;
  char *value;
  GtkWidget *row, *shortcut, *entry, *stack;
} Shortcut;

struct _Editor {
  AppWindow *win;
  GtkWidget *window, *profiles, *plugins, *name, *command, *arguments, *model;
  GtkWidget *fields, *form, *status, *test, *save, *discover, *remove,
      *model_choices;
  GtkWidget *heading, *advanced_fields, *advanced, *add, *revert, *stop,
      *spinner, *plugin_row;
  GtkWidget *sections, *query_font, *answer_font, *numbers;
  gboolean editor_dirty, shortcuts_dirty, close_after_apply;
  GtkWidget *ok, *shortcut_list, *shortcut_search, *shortcut_change;
  GPtrArray *shortcuts;
  GtkSizeGroup *provider_labels;
  GPtrArray *profile_list;
  GHashTable *inputs;
  ProviderProfile *profile;
  Provider *worker;
  GCancellable *cancel;
  gboolean updating, busy, closed, discovery;
  gboolean dirty;
  gboolean confirming_close, allow_close;
  guint progress_source;
  gint64 started;
};

static void editor_fill(Editor *e);
static void rebuild_profiles(Editor *e);
static void update_apply(Editor *e);
static gboolean apply_options(Editor *e);
static void shortcut_free(gpointer data);
static gboolean shortcut_assign(Shortcut *shortcut, const char *text);

static void editor_free(gpointer data) {
  Editor *e = data;
  if (e->progress_source != 0)
    g_source_remove(e->progress_source);
  if (e->worker != NULL)
    provider_manager_destroy(e->worker);
  g_clear_object(&e->cancel);
  g_clear_pointer(&e->profile_list, g_ptr_array_unref);
  g_hash_table_unref(e->inputs);
  g_clear_pointer(&e->shortcuts, g_ptr_array_unref);
  g_clear_object(&e->provider_labels);
  provider_profile_free(e->profile);
  g_free(e);
}

static void set_status(Editor *e, const char *message, gboolean error) {
  gtk_label_set_text(GTK_LABEL(e->status), message);
  gtk_widget_set_tooltip_text(e->status, message);
  if (error)
    gtk_widget_add_css_class(e->status, "error");
  else
    gtk_widget_remove_css_class(e->status, "error");
}

static void error_close(GtkButton *button, GtkWindow *dialog) {
  (void)button;
  gtk_window_destroy(dialog);
}

static gboolean error_key_pressed(GtkEventControllerKey *controller,
                                  guint keyval, guint keycode,
                                  GdkModifierType state, GtkWindow *dialog) {
  (void)controller;
  (void)keycode;
  (void)state;
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;
  gtk_window_destroy(dialog);
  return GDK_EVENT_STOP;
}

static void request_failed(Editor *e, gboolean discovery, const char *detail) {
  const char *title =
      discovery ? "Could not load models" : "Connection test failed";
  set_status(e, title, TRUE);
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               app_window_dialog_parent(GTK_WINDOW(e->window)));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 360);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(content), scroll);
  GtkWidget *text = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 8);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text), 8);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(text), GTK_ACCESSIBLE_PROPERTY_LABEL, "Error details", -1);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),
                           detail != NULL ? detail : "Request failed", -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
  GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(content), close);
  gtk_window_set_default_widget(GTK_WINDOW(dialog), close);
  g_signal_connect(close, "clicked", G_CALLBACK(error_close), dialog);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(error_key_pressed), dialog);
  gtk_widget_add_controller(dialog, keys);
  gtk_window_present(GTK_WINDOW(dialog));
}

static void invalidate(Editor *e) {
  if (e->updating)
    return;
  e->profile->tested = FALSE;
  e->dirty = TRUE;
  gtk_widget_set_visible(e->revert, TRUE);
  gtk_widget_set_sensitive(e->remove, FALSE);
  update_apply(e);
  set_status(e, "Test the connection before applying changes.", FALSE);
}
static void field_changed(GtkEditable *field, Editor *e) {
  if (!e->updating && GTK_WIDGET(field) == e->model) {
    const char *text = gtk_editable_get_text(field);
    e->updating = TRUE;
    if (*text)
      set_selected_text(e->model_choices, text);
    else
      gtk_drop_down_set_selected(GTK_DROP_DOWN(e->model_choices), 0);
    e->updating = FALSE;
    gtk_widget_set_sensitive(e->model_choices,
                             g_list_model_get_n_items(gtk_drop_down_get_model(
                                 GTK_DROP_DOWN(e->model_choices))) > 1);
  }
  invalidate(e);
}
static void name_changed(GtkEditable *field, Editor *e) {
  if (e->updating)
    return;
  g_free(e->profile->name);
  e->profile->name = g_strdup(gtk_editable_get_text(field));
  e->dirty = TRUE;
  gtk_label_set_text(GTK_LABEL(e->heading), e->profile->name);
  gtk_widget_set_visible(e->revert, TRUE);
  gtk_widget_set_sensitive(e->remove, FALSE);
  update_apply(e);
}
static void choice_changed(GObject *object, GParamSpec *pspec, Editor *e) {
  (void)object;
  (void)pspec;
  invalidate(e);
}
static void toggle_changed(GtkCheckButton *button, Editor *e) {
  (void)button;
  invalidate(e);
}

static void busy(Editor *e, gboolean value) {
  if (value)
    gtk_window_set_focus(GTK_WINDOW(e->window), NULL);
  e->busy = value;
  gtk_widget_set_sensitive(e->form, !value);
  gtk_widget_set_sensitive(e->profiles, !value);
  gtk_widget_set_sensitive(e->add, !value);
  gtk_widget_set_sensitive(e->test, !value);
  gtk_widget_set_sensitive(e->discover, !value);
  gboolean saved = FALSE;
  for (guint i = 0; e->profile_list != NULL && i < e->profile_list->len; i++)
    if (g_strcmp0(
            ((ProviderProfile *)g_ptr_array_index(e->profile_list, i))->id,
            e->profile->id) == 0)
      saved = TRUE;
  gtk_widget_set_sensitive(e->remove, !value && saved && !e->profile->bundled &&
                                          !e->dirty);
  PluginManifest *manifest = plugin_manager_get_manifest(e->profile->plugin);
  gtk_button_set_label(
      GTK_BUTTON(e->remove),
      manifest != NULL && g_strcmp0(manifest->profile_id, e->profile->id) == 0
          ? "Reset to defaults"
          : "Remove provider");
  update_apply(e);
  gtk_widget_set_visible(e->stop, value && e->worker != NULL);
  gtk_widget_set_sensitive(e->stop, value);
  gtk_widget_set_sensitive(e->revert, !value);
  gtk_widget_set_visible(e->spinner, value);
  if (value)
    gtk_spinner_start(GTK_SPINNER(e->spinner));
  else {
    gtk_spinner_stop(GTK_SPINNER(e->spinner));
    if (e->progress_source != 0) {
      g_source_remove(e->progress_source);
      e->progress_source = 0;
    }
  }
  if (value && e->worker != NULL)
    gtk_widget_grab_focus(e->stop);
}

static void discard_close_ready(GObject *source, GAsyncResult *result,
                                gpointer data) {
  Editor *e = data;
  int choice =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
  e->confirming_close = FALSE;
  if (!e->closed && choice == 1) {
    e->allow_close = TRUE;
    gtk_window_close(GTK_WINDOW(e->window));
  }
  g_object_unref(e->window);
}

static gboolean close_editor(GtkWindow *window, Editor *e) {
  if ((e->dirty || e->editor_dirty || e->shortcuts_dirty) && !e->busy &&
      !e->allow_close && gtk_widget_get_visible(e->window)) {
    if (!e->confirming_close) {
      e->confirming_close = TRUE;
      g_autoptr(GtkAlertDialog) dialog =
          gtk_alert_dialog_new("Discard unsaved changes?");
      gtk_alert_dialog_set_detail(dialog,
                                  "Saved preferences will stay unchanged.");
      const char *buttons[] = {"Keep editing", "Discard changes", NULL};
      gtk_alert_dialog_set_modal(dialog, TRUE);
      gtk_alert_dialog_set_buttons(dialog, buttons);
      gtk_alert_dialog_set_cancel_button(dialog, 0);
      gtk_alert_dialog_set_default_button(dialog, 0);
      g_object_ref(e->window);
      gtk_alert_dialog_choose(dialog, app_window_dialog_parent(window), NULL,
                              discard_close_ready, e);
    }
    return TRUE;
  }
  gtk_window_set_focus(window, NULL);
  e->closed = TRUE;
  if (e->cancel != NULL)
    g_cancellable_cancel(e->cancel);
  return FALSE;
}

static void visibility_changed(GObject *object, GParamSpec *pspec, Editor *e) {
  (void)pspec;
  if (!gtk_widget_get_visible(GTK_WIDGET(object))) {
    e->closed = TRUE;
    if (e->cancel != NULL)
      g_cancellable_cancel(e->cancel);
  }
}

static GtkWidget *entry_row(Editor *e, GtkWidget *box, const char *label_text) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *label = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_size_request(label, 100, -1);
  gtk_size_group_add_widget(e->provider_labels, label);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 14);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
  gtk_box_append(GTK_BOX(row), label);
  gtk_box_append(GTK_BOX(row), entry);
  gtk_box_append(GTK_BOX(box), row);
  return entry;
}

static char *selected(GtkWidget *dropdown) {
  return get_selected_text(dropdown);
}

static void model_item_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *item, gpointer data) {
  (void)factory;
  (void)data;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 32);
  gtk_list_item_set_child(item, label);
}

static void model_item_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *item, gpointer data) {
  (void)factory;
  (void)data;
  GtkWidget *label = gtk_list_item_get_child(item);
  gpointer object = gtk_list_item_get_item(item);
  if (GTK_IS_LABEL(label) && GTK_IS_STRING_OBJECT(object)) {
    const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(object));
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_tooltip_text(label, text);
  }
}

static JsonObject *schema_for(Editor *e) {
  PluginManifest *manifest = plugin_manager_get_manifest(e->profile->plugin);
  return manifest != NULL && manifest->config_schema != NULL &&
                 JSON_NODE_HOLDS_OBJECT(manifest->config_schema)
             ? json_node_get_object(manifest->config_schema)
             : NULL;
}

static void editor_fill(Editor *e) {
  gtk_window_set_focus(GTK_WINDOW(e->window), NULL);
  e->updating = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(e->name), e->profile->name);
  gtk_editable_set_text(GTK_EDITABLE(e->command), e->profile->command);
  gtk_editable_set_text(GTK_EDITABLE(e->model),
                        e->profile->model != NULL ? e->profile->model : "");
  g_autoptr(GString) args = g_string_new(NULL);
  for (guint i = 0;
       e->profile->arguments != NULL && e->profile->arguments[i] != NULL; i++) {
    g_autofree char *quoted = g_shell_quote(e->profile->arguments[i]);
    if (i)
      g_string_append_c(args, ' ');
    g_string_append(args, quoted);
  }
  gtk_editable_set_text(GTK_EDITABLE(e->arguments), args->str);
  set_selected_text(e->plugins, e->profile->plugin);
  g_hash_table_remove_all(e->inputs);
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(e->fields)) != NULL)
    gtk_box_remove(GTK_BOX(e->fields), child);
  while ((child = gtk_widget_get_first_child(e->advanced_fields)) != NULL)
    gtk_box_remove(GTK_BOX(e->advanced_fields), child);
  JsonObject *schema = schema_for(e);
  g_autoptr(GList) keys =
      schema != NULL ? json_object_get_members(schema) : NULL;
  for (GList *it = keys; it != NULL; it = it->next) {
    const char *key = it->data;
    JsonObject *field = protocol_object(schema, key);
    const char *type = protocol_string(field, "type");
    const char *comment = protocol_string(field, "comment");
    const char *label = protocol_string(field, "label");
    if (label == NULL)
      label = comment != NULL ? comment : key;
    GtkWidget *group =
        protocol_boolean(field, "advanced") ? e->advanced_fields : e->fields;
    GtkWidget *input;
    if (g_strcmp0(type, "boolean") == 0) {
      input = gtk_check_button_new_with_label(label);
      gtk_check_button_set_active(
          GTK_CHECK_BUTTON(input),
          json_object_has_member(e->profile->settings, key)
              ? protocol_boolean(e->profile->settings, key)
              : protocol_boolean(field, "default"));
      gtk_box_append(GTK_BOX(group), input);
      g_signal_connect(input, "toggled", G_CALLBACK(toggle_changed), e);
    } else if (g_strcmp0(type, "choice") == 0) {
      JsonNode *choices =
          field != NULL ? json_object_get_member(field, "choices") : NULL;
      GtkStringList *list = gtk_string_list_new(NULL);
      if (choices != NULL && JSON_NODE_HOLDS_ARRAY(choices)) {
        JsonArray *a = json_node_get_array(choices);
        for (guint i = 0; i < json_array_get_length(a); i++) {
          JsonNode *v = json_array_get_element(a, i);
          if (JSON_NODE_HOLDS_VALUE(v) &&
              json_node_get_value_type(v) == G_TYPE_STRING)
            gtk_string_list_append(list, json_node_get_string(v));
        }
      }
      GtkWidget *field_label = gtk_label_new(label);
      gtk_widget_set_halign(field_label, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(group), field_label);
      input = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
      gtk_box_append(GTK_BOX(group), input);
      const char *choice = protocol_string(e->profile->settings, key);
      if (choice == NULL)
        choice = protocol_string(field, "default");
      set_selected_text(input, choice);
      g_signal_connect(input, "notify::selected", G_CALLBACK(choice_changed),
                       e);
    } else {
      input = entry_row(e, group, label);
      gboolean secret = protocol_boolean(field, "secret");
      if (secret)
        gtk_entry_set_input_purpose(GTK_ENTRY(input),
                                    GTK_INPUT_PURPOSE_PASSWORD);
      gtk_entry_set_visibility(GTK_ENTRY(input), !secret);
      const char *value = protocol_string(e->profile->settings, key);
      if (value == NULL)
        value = protocol_string(field, "default");
      if (value != NULL)
        gtk_editable_set_text(GTK_EDITABLE(input), value);
      if (g_strcmp0(type, "number") == 0 && value == NULL) {
        JsonNode *number = json_object_get_member(e->profile->settings, key);
        if (number == NULL)
          number = json_object_get_member(field, "default");
        if (number != NULL && JSON_NODE_HOLDS_VALUE(number) &&
            (json_node_get_value_type(number) == G_TYPE_DOUBLE ||
             json_node_get_value_type(number) == G_TYPE_INT64)) {
          g_autofree char *text = json_to_string(number, FALSE);
          gtk_editable_set_text(GTK_EDITABLE(input), text);
        }
      }
      if (secret)
        gtk_entry_set_placeholder_text(GTK_ENTRY(input),
                                       "Leave blank to keep saved key");
      g_signal_connect(input, "changed", G_CALLBACK(field_changed), e);
    }
    g_hash_table_insert(e->inputs, g_strdup(key), input);
  }
  gtk_label_set_text(GTK_LABEL(e->heading), e->profile->name);
  PluginManifest *manifest = plugin_manager_get_manifest(e->profile->plugin);
  gtk_widget_set_tooltip_text(e->fields,
                              manifest != NULL ? manifest->setup_hint : NULL);
  gtk_widget_set_visible(gtk_widget_get_parent(e->fields),
                         gtk_widget_get_first_child(e->fields) != NULL);
  GtkStringList *choices = gtk_string_list_new(NULL);
  gtk_string_list_append(choices, "Use backend default");
  char **models =
      e->win->model_catalogs != NULL
          ? g_hash_table_lookup(e->win->model_catalogs, e->profile->id)
          : NULL;
  if (models == NULL)
    models = e->profile->models;
  for (guint i = 0; models != NULL && models[i] != NULL; i++)
    if (*models[i])
      gtk_string_list_append(choices, models[i]);
  gtk_drop_down_set_model(GTK_DROP_DOWN(e->model_choices),
                          G_LIST_MODEL(choices));
  gtk_widget_set_sensitive(e->model_choices,
                           g_list_model_get_n_items(G_LIST_MODEL(choices)) > 1);
  g_object_unref(choices);
  if (e->profile->model != NULL && *e->profile->model)
    set_selected_text(e->model_choices, e->profile->model);
  else
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->model_choices), 0);
  e->dirty = FALSE;
  gtk_widget_set_visible(e->revert, FALSE);
  e->updating = FALSE;
  busy(e, FALSE);
  gboolean saved_profile = FALSE;
  for (guint i = 0; e->profile_list != NULL && i < e->profile_list->len; i++)
    saved_profile |=
        g_strcmp0(
            ((ProviderProfile *)g_ptr_array_index(e->profile_list, i))->id,
            e->profile->id) == 0;
  gtk_widget_set_sensitive(e->plugins, !saved_profile);
  gtk_widget_set_visible(e->plugin_row, !saved_profile);
  set_status(
      e,
      e->profile->bundled  ? "Test the connection before applying changes."
      : e->profile->tested ? "Settings saved."
                           : "Test the connection before applying changes.",
      FALSE);
}

static ProviderProfile *collect(Editor *e, GError **error) {
  ProviderProfile *p = provider_profile_copy(e->profile);
  g_autoptr(GPtrArray) secrets = g_ptr_array_new_with_free_func(g_free);
  g_free(p->name);
  p->name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e->name)));
  g_free(p->command);
  p->command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e->command)));
  g_free(p->model);
  p->model = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e->model)));
  if (!*p->name || !*p->command) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Enter a profile name and executable");
    goto failed;
  }
  g_strfreev(p->arguments);
  p->arguments = NULL;
  const char *args = gtk_editable_get_text(GTK_EDITABLE(e->arguments));
  if (*args && !g_shell_parse_argv(args, NULL, &p->arguments, error))
    goto failed;
  JsonObject *schema = schema_for(e);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, e->inputs);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    JsonObject *field = protocol_object(schema, key);
    if (protocol_boolean(field, "secret"))
      g_ptr_array_add(secrets, g_strdup(key));
    if (GTK_IS_CHECK_BUTTON(value))
      json_object_set_boolean_member(p->settings, key,
                                     gtk_check_button_get_active(value));
    else {
      g_autofree char *choice =
          GTK_IS_DROP_DOWN(value) ? selected(value) : NULL;
      const char *text =
          choice != NULL ? choice : gtk_editable_get_text(GTK_EDITABLE(value));
      const char *type = protocol_string(field, "type");
      if (g_strcmp0(type, "number") == 0) {
        char *end = NULL;
        double number = g_ascii_strtod(text, &end);
        if (!*text || end == NULL || *end) {
          g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "Enter a number for %s", (char *)key);
          goto failed;
        }
        json_object_set_double_member(p->settings, key, number);
      } else if (*text)
        json_object_set_string_member(p->settings, key, text);
      else
        json_object_remove_member(p->settings, key);
    }
  }
  g_ptr_array_add(secrets, NULL);
  g_strfreev(p->secret_fields);
  p->secret_fields =
      (char **)g_ptr_array_free(g_steal_pointer(&secrets), FALSE);
  return p;
failed:
  provider_profile_free(p);
  return NULL;
}

static void test_event(gpointer source, const ProviderEvent *event,
                       gpointer data) {
  (void)data;
  Editor *e = source;
  if (event->type == PROVIDER_EVENT_CHUNK)
    return;
  if (e->worker != NULL) {
    provider_manager_destroy(e->worker);
    e->worker = NULL;
  }
  if (!e->closed) {
    if (event->type == PROVIDER_EVENT_DONE) {
      if (e->discovery) {
        e->dirty = TRUE;
        gtk_widget_set_visible(e->revert, TRUE);
        g_strfreev(e->profile->models);
        e->profile->models = g_strdupv(event->models);
        g_autofree char *message = g_strdup_printf(
            "Loaded %u models. Choose a default, or use the "
            "backend default.",
            event->models != NULL ? g_strv_length(event->models) : 0);
        set_status(e, message, FALSE);
        e->updating = TRUE;
        GtkStringList *list = gtk_string_list_new(NULL);
        gtk_string_list_append(list, "Use backend default");
        for (guint i = 0; event->models != NULL && event->models[i] != NULL;
             i++)
          gtk_string_list_append(list, event->models[i]);
        gtk_drop_down_set_model(GTK_DROP_DOWN(e->model_choices),
                                G_LIST_MODEL(list));
        g_object_unref(list);
        gtk_widget_set_visible(e->model_choices, TRUE);
        gtk_widget_set_sensitive(
            e->model_choices, g_list_model_get_n_items(gtk_drop_down_get_model(
                                  GTK_DROP_DOWN(e->model_choices))) > 1);
        e->updating = FALSE;

      } else {
        e->profile->tested = TRUE;
        set_status(e, "Connected. Click Apply to save.", FALSE);
      }
    } else if (event->error_msg != NULL && *event->error_msg)
      request_failed(e, e->discovery, event->error_msg);
    else
      set_status(e,
                 e->discovery ? "Model discovery cancelled."
                              : "Connection test cancelled.",
                 FALSE);
    busy(e, FALSE);
  }
  g_object_unref(e->window);
}

static gboolean progress_tick(gpointer data) {
  Editor *e = data;
  if (e->closed || !e->busy || e->worker == NULL) {
    e->progress_source = 0;
    return G_SOURCE_REMOVE;
  }
  if (!g_cancellable_is_cancelled(e->cancel)) {
    gint64 seconds = (g_get_monotonic_time() - e->started) / G_TIME_SPAN_SECOND;
    g_autofree char *message = g_strdup_printf(
        "%s · %" G_GINT64_FORMAT "s elapsed (60s timeout)",
        e->discovery ? "Loading models" : "Testing connection", seconds);
    set_status(e, message, FALSE);
  }
  return G_SOURCE_CONTINUE;
}

static void start_request(Editor *e, gboolean discovery) {
  if (e->busy)
    return;
  g_autoptr(GError) error = NULL;
  ProviderProfile *p = collect(e, &error);
  if (p == NULL) {
    request_failed(e, discovery, error->message);
    return;
  }
  if (!discovery) {
    p->tested = FALSE;
    e->dirty = TRUE;
    gtk_widget_set_visible(e->revert, TRUE);
  }
  provider_profile_free(e->profile);
  e->profile = p;
  g_clear_object(&e->cancel);
  e->cancel = g_cancellable_new();
  e->worker = plugin_runner_create_profile(p);
  e->discovery = discovery;
  g_autofree char *thread_id = g_uuid_string_random(),
                  *request_id = g_uuid_string_random();
  ProviderRequest q = {.thread_id = thread_id,
                       .request_id = request_id,
                       .query = "Reply briefly: pong.",
                       .model = p->model,
                       .connection_test = !discovery,
                       .discover_models = discovery};
  busy(e, TRUE);
  set_status(e, discovery ? "Discovering models…" : "Testing connection…",
             FALSE);
  e->started = g_get_monotonic_time();
  e->progress_source = g_timeout_add_seconds(1, progress_tick, e);
  g_object_ref(e->window);
  if (!e->worker->vtable->submit(e->worker->impl, e, &q, e->cancel, test_event,
                                 NULL, &error)) {
    request_failed(e, discovery, error->message);
    busy(e, FALSE);
    g_object_unref(e->window);
    provider_manager_destroy(e->worker);
    e->worker = NULL;
  }
}
static void test_clicked(GtkButton *button, Editor *e) {
  (void)button;
  start_request(e, FALSE);
}
static void discover_clicked(GtkButton *button, Editor *e) {
  (void)button;
  start_request(e, TRUE);
}

typedef struct {
  ProviderProfile *profile;
  gboolean remove;
} SaveJob;
static void save_job_free(SaveJob *job) {
  provider_profile_free(job->profile);
  g_free(job);
}
static void save_worker(GTask *task, gpointer source, gpointer data,
                        GCancellable *cancel) {
  (void)source;
  SaveJob *job = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = job->remove
                    ? provider_profile_delete(job->profile, cancel, &error)
                    : provider_profile_save(job->profile, cancel, &error);
  if (ok)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, g_steal_pointer(&error));
}
static void save_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source;
  Editor *e = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
  if (!e->closed) {
    if (ok) {
      SaveJob *job = g_task_get_task_data(G_TASK(result));
      ProviderProfile *saved = provider_profile_load(e->profile->id, NULL);
      if (saved == NULL && job->remove)
        saved = provider_profile_new(e->profile->plugin);
      if (saved != NULL) {
        provider_profile_free(e->profile);
        e->profile = saved;
        editor_fill(e);
      }
      app_window_refresh_models(e->win);
      rebuild_profiles(e);
      set_status(e,
                 job->remove ? "Provider settings removed." : "Changes saved.",
                 FALSE);
    } else
      set_status(e,
                 error != NULL ? error->message
                               : "Could not save provider; unlock the system "
                                 "keyring and try again.",
                 TRUE);
    busy(e, FALSE);
    if (ok && e->close_after_apply) {
      e->allow_close = TRUE;
      gtk_window_close(GTK_WINDOW(e->window));
    } else if (!ok)
      e->close_after_apply = FALSE;
  }
  g_object_unref(e->window);
}
static void save_or_remove(Editor *e, gboolean remove) {
  if (e->busy || (!remove && !e->profile->tested))
    return;
  if (remove) {
    for (guint i = 0; i < e->win->tabs->len; i++) {
      Tab *tab = g_ptr_array_index(e->win->tabs, i);
      if (tab->state == STATE_LOADING &&
          g_strcmp0(tab->request_profile, e->profile->id) == 0) {
        set_status(e,
                   "This profile has a pending reply. Wait for it to finish or "
                   "cancel it before deleting the profile.",
                   TRUE);
        return;
      }
    }
  }
  SaveJob *job = g_new0(SaveJob, 1);
  job->profile = provider_profile_copy(e->profile);
  job->remove = remove;
  g_clear_object(&e->cancel);
  e->cancel = g_cancellable_new();
  busy(e, TRUE);
  set_status(e, remove ? "Deleting profile…" : "Saving profile…", FALSE);
  g_autoptr(GTask) task = g_task_new(NULL, e->cancel, save_ready, e);
  g_task_set_task_data(task, job, (GDestroyNotify)save_job_free);
  g_object_ref(e->window);
  g_task_run_in_thread(task, save_worker);
}
static void save_clicked(GtkButton *button, Editor *e) {
  (void)button;
  if (e->busy || (e->dirty && !e->profile->tested)) {
    e->close_after_apply = FALSE;
    return;
  }
  if ((e->editor_dirty || e->shortcuts_dirty) && !apply_options(e)) {
    e->close_after_apply = FALSE;
    return;
  }
  if (e->dirty)
    save_or_remove(e, FALSE);
  else if (e->close_after_apply) {
    e->allow_close = TRUE;
    gtk_window_close(GTK_WINDOW(e->window));
  }
}
static void delete_ready(GObject *source, GAsyncResult *result, gpointer data) {
  Editor *e = data;
  int choice =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
  if (!e->closed && choice == 1)
    save_or_remove(e, TRUE);
  g_object_unref(e->window);
}
static void delete_clicked(GtkButton *button, Editor *e) {
  (void)button;
  if (e->busy || e->dirty)
    return;
  PluginManifest *manifest = plugin_manager_get_manifest(e->profile->plugin);
  gboolean reset =
      manifest != NULL && g_strcmp0(manifest->profile_id, e->profile->id) == 0;
  g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new(
      "%s", reset ? "Reset provider to defaults?" : "Remove provider?");
  gtk_alert_dialog_set_detail(
      dialog, "Saved settings and keyring credentials for this profile will be "
              "removed. Conversations are kept.");
  const char *buttons[] = {"Cancel", reset ? "Reset" : "Remove", NULL};
  gtk_alert_dialog_set_modal(dialog, TRUE);
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  g_object_ref(e->window);
  gtk_alert_dialog_choose(dialog,
                          app_window_dialog_parent(GTK_WINDOW(e->window)), NULL,
                          delete_ready, e);
}

static void restore_profile_selection(Editor *e) {
  e->updating = TRUE;
  gtk_list_box_unselect_all(GTK_LIST_BOX(e->profiles));
  for (guint i = 0; e->profile_list != NULL && i < e->profile_list->len; i++) {
    ProviderProfile *p = g_ptr_array_index(e->profile_list, i);
    if (g_strcmp0(p->id, e->profile->id) == 0)
      gtk_list_box_select_row(
          GTK_LIST_BOX(e->profiles),
          gtk_list_box_get_row_at_index(GTK_LIST_BOX(e->profiles), i));
  }
  e->updating = FALSE;
}

static gboolean can_switch(Editor *e) {
  if (e->dirty) {
    restore_profile_selection(e);
    set_status(e, "Apply or discard changes before switching providers.",
               FALSE);
    return FALSE;
  }
  return !e->busy;
}

static void profile_changed(GtkListBox *box, GtkListBoxRow *row, Editor *e) {
  (void)box;
  if (e->updating || row == NULL || !can_switch(e))
    return;
  int position = gtk_list_box_row_get_index(row);
  if (position < 0 || (guint)position >= e->profile_list->len)
    return;
  ProviderProfile *p =
      provider_profile_copy(g_ptr_array_index(e->profile_list, position));
  provider_profile_free(e->profile);
  e->profile = p;
  editor_fill(e);
}

static void add_clicked(GtkButton *button, Editor *e) {
  (void)button;
  if (!can_switch(e))
    return;
  ProviderProfile *p = provider_profile_new(e->profile->plugin);
  provider_profile_free(e->profile);
  e->profile = p;
  restore_profile_selection(e);
  editor_fill(e);
  gtk_widget_grab_focus(e->name);
}

static void revert_clicked(GtkButton *button, Editor *e) {
  (void)button;
  ProviderProfile *p = provider_profile_load(e->profile->id, NULL);
  if (p == NULL)
    p = provider_profile_new(e->profile->plugin);
  provider_profile_free(e->profile);
  e->profile = p;
  restore_profile_selection(e);
  editor_fill(e);
}

static void stop_clicked(GtkButton *button, Editor *e) {
  (void)button;
  if (e->cancel != NULL)
    g_cancellable_cancel(e->cancel);
  gtk_widget_set_sensitive(e->stop, FALSE);
  set_status(e, "Cancelling…", FALSE);
}

static gboolean preferences_key_pressed(GtkEventControllerKey *controller,
                                        guint keyval, guint keycode,
                                        GdkModifierType state, Editor *e) {
  (void)controller;
  (void)keycode;
  (void)state;
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;
  if (e->busy && e->worker != NULL)
    stop_clicked(NULL, e);
  else
    gtk_window_close(GTK_WINDOW(e->window));
  return GDK_EVENT_STOP;
}

static void plugin_changed(GObject *object, GParamSpec *pspec, Editor *e) {
  (void)object;
  (void)pspec;
  if (e->updating || e->busy)
    return;
  if (e->dirty) {
    e->updating = TRUE;
    set_selected_text(e->plugins, e->profile->plugin);
    e->updating = FALSE;
    set_status(e, "Apply or discard changes before changing provider type.",
               FALSE);
    return;
  }
  g_autofree char *name = selected(e->plugins);
  provider_profile_free(e->profile);
  e->profile = provider_profile_new(name);
  restore_profile_selection(e);
  editor_fill(e);
  gtk_expander_set_expanded(GTK_EXPANDER(e->advanced),
                            g_strcmp0(name, "custom") == 0);
}

static void rebuild_profiles(Editor *e) {
  e->updating = TRUE;
  g_clear_pointer(&e->profile_list, g_ptr_array_unref);
  e->profile_list = provider_profile_list();
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(e->profiles)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(e->profiles), child);
  for (guint i = 0; i < e->profile_list->len; i++) {
    ProviderProfile *p = g_ptr_array_index(e->profile_list, i);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    GtkWidget *name = gtk_label_new(p->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 22);

    gtk_box_append(GTK_BOX(row), name);
    gtk_list_box_append(GTK_LIST_BOX(e->profiles), row);
  }
  e->updating = FALSE;
  restore_profile_selection(e);
}

static void model_choice_changed(GObject *object, GParamSpec *pspec,
                                 Editor *e) {
  (void)object;
  (void)pspec;
  if (e->updating)
    return;
  guint position = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->model_choices));
  g_autofree char *value = selected(e->model_choices);
  e->updating = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(e->model), position == 0 ? "" : value);
  e->updating = FALSE;
  invalidate(e);
}

static void editor_option_changed(GObject *object, GParamSpec *pspec,
                                  Editor *e) {
  (void)object;
  (void)pspec;
  e->editor_dirty = TRUE;
  update_apply(e);
}

static void editor_numbers_changed(GtkCheckButton *button, Editor *e) {
  editor_option_changed(G_OBJECT(button), NULL, e);
}

static gboolean apply_options(Editor *e) {
  for (guint i = 0; e->shortcuts != NULL && i < e->shortcuts->len; i++) {
    Shortcut *shortcut = g_ptr_array_index(e->shortcuts, i);
    if (gtk_stack_get_visible_child(GTK_STACK(shortcut->stack)) ==
        shortcut->entry) {
      if (!shortcut_assign(
              shortcut, gtk_editable_get_text(GTK_EDITABLE(shortcut->entry))))
        return FALSE;
      gtk_stack_set_visible_child_name(GTK_STACK(shortcut->stack), "label");
    }
  }
  const char *keys[] = {"query_font", "answer_font", "editor_line_numbers"};
  const PangoFontDescription *query = gtk_font_dialog_button_get_font_desc(
      GTK_FONT_DIALOG_BUTTON(e->query_font));
  const PangoFontDescription *answer = gtk_font_dialog_button_get_font_desc(
      GTK_FONT_DIALOG_BUTTON(e->answer_font));
  g_autofree char *query_name = pango_font_description_to_string(query);
  g_autofree char *answer_name = pango_font_description_to_string(answer);
  const char *values[] = {
      query_name, answer_name,
      gtk_check_button_get_active(GTK_CHECK_BUTTON(e->numbers)) ? "1" : "0"};
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) all_keys = g_ptr_array_new();
  g_autoptr(GPtrArray) all_values = g_ptr_array_new();
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    g_ptr_array_add(all_keys, (gpointer)keys[i]);
    g_ptr_array_add(all_values, (gpointer)values[i]);
  }
  if (e->shortcuts_dirty && e->shortcuts != NULL)
    for (guint i = 0; i < e->shortcuts->len; i++) {
      Shortcut *shortcut = g_ptr_array_index(e->shortcuts, i);
      g_ptr_array_add(all_keys, (gpointer)shortcut->key);
      g_ptr_array_add(all_values, shortcut->value);
    }
  if (!runtime_config_save_values(
          e->win->config, (const char *const *)all_keys->pdata,
          (const char *const *)all_values->pdata, all_keys->len, &error)) {
    show_error_dialog(GTK_WINDOW(e->window), "Could not save preferences",
                      error->message);
    return FALSE;
  }
  app_window_apply_editor_preferences(e->win);
  if (e->shortcuts_dirty)
    app_window_reload_shortcuts(e->win);
  e->editor_dirty = FALSE;
  e->shortcuts_dirty = FALSE;
  update_apply(e);
  return TRUE;
}

static void grid_setting(GtkWidget *grid, int row, const char *text,
                         GtkWidget *control) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_mnemonic_widget(GTK_LABEL(label), control);
  gtk_widget_set_hexpand(control, TRUE);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(control),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

static GtkWidget *font_setting(Editor *e, const char *key, const char *fallback,
                               const char *title) {
  GtkFontDialog *dialog = gtk_font_dialog_new();
  gtk_font_dialog_set_title(dialog, title);
  GtkWidget *button = gtk_font_dialog_button_new(dialog);
  gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(button),
                                   GTK_FONT_LEVEL_FONT);
  g_autofree char *value =
      runtime_config_get_string(e->win->config, key, fallback);
  g_autoptr(PangoFontDescription) font =
      pango_font_description_from_string(value);
  if (pango_font_description_get_size(font) <= 0) {
    g_autofree char *system_name = NULL;
    g_object_get(gtk_widget_get_settings(e->window), "gtk-font-name",
                 &system_name, NULL);
    g_autoptr(PangoFontDescription) system_font =
        pango_font_description_from_string(system_name != NULL ? system_name
                                                               : "Sans 11");
    pango_font_description_set_size(
        font, pango_font_description_get_size(system_font));
  }
  gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), font);
  g_signal_connect(button, "notify::font-desc",
                   G_CALLBACK(editor_option_changed), e);
  return button;
}

static GtkWidget *editor_page(Editor *e) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_box_append(GTK_BOX(box), grid);
  e->query_font =
      font_setting(e, "query_font", QUERY_FONT_DEFAULT, "Query font");
  e->answer_font =
      font_setting(e, "answer_font", ANSWER_FONT_DEFAULT, "Answer font");
  grid_setting(grid, 0, "Query font:", e->query_font);
  grid_setting(grid, 1, "Answer font:", e->answer_font);
  e->numbers = gtk_check_button_new_with_label("Show line numbers");
  gtk_check_button_set_active(
      GTK_CHECK_BUTTON(e->numbers),
      runtime_config_get_bool(e->win->config, "editor_line_numbers",
                              EDITOR_LINE_NUMBERS_DEFAULT));
  gtk_grid_attach(GTK_GRID(grid), e->numbers, 1, 2, 1, 1);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(box), spacer);
  g_signal_connect(e->numbers, "toggled", G_CALLBACK(editor_numbers_changed),
                   e);

  return box;
}

static void update_apply(Editor *e) {
  if (!GTK_IS_BUTTON(e->save))
    return;
  gboolean valid =
      !e->busy &&
      (!e->dirty || (e->profile != NULL && e->profile->tested &&
                     e->profile->name != NULL && *e->profile->name));
  gtk_widget_set_sensitive(
      e->save, valid && (e->dirty || e->editor_dirty || e->shortcuts_dirty));
  if (GTK_IS_BUTTON(e->ok))
    gtk_widget_set_sensitive(e->ok, valid);
}

static void cancel_preferences(GtkButton *button, Editor *e) {
  (void)button;
  e->allow_close = TRUE;
  gtk_window_close(GTK_WINDOW(e->window));
}

static void ok_preferences(GtkButton *button, Editor *e) {
  e->close_after_apply = TRUE;
  save_clicked(button, e);
}

static void shortcut_free(gpointer data) {
  Shortcut *shortcut = data;
  g_free(shortcut->value);
  g_free(shortcut);
}

static void shortcuts_recompute_dirty(Editor *e) {
  e->shortcuts_dirty = FALSE;
  for (guint i = 0; i < e->shortcuts->len; i++) {
    Shortcut *shortcut = g_ptr_array_index(e->shortcuts, i);
    g_autofree char *saved = runtime_config_get_string(
        e->win->config, shortcut->key, shortcut->fallback);
    const char *value =
        gtk_stack_get_visible_child(GTK_STACK(shortcut->stack)) ==
                shortcut->entry
            ? gtk_editable_get_text(GTK_EDITABLE(shortcut->entry))
            : shortcut->value;
    if (g_strcmp0(saved, value) != 0)
      e->shortcuts_dirty = TRUE;
  }
  update_apply(e);
}

static gboolean shortcut_assign(Shortcut *shortcut, const char *text) {
  Editor *e = shortcut->editor;
  guint key = 0;
  GdkModifierType mods = 0;
  if (*text &&
      (!gtk_accelerator_parse(text, &key, &mods) ||
       !gtk_accelerator_valid(key, mods) || key == GDK_KEY_Escape ||
       (mods == GDK_ALT_MASK && key >= GDK_KEY_1 && key <= GDK_KEY_9))) {
    show_error_dialog(
        GTK_WINDOW(e->window), "Invalid shortcut",
        "Enter a valid shortcut. esc and alt+1 through alt+9 are reserved.");
    return FALSE;
  }
  g_autofree char *canonical =
      *text ? gtk_accelerator_name(key, mods) : g_strdup("");
  for (guint i = 0; *text && i < e->shortcuts->len; i++) {
    Shortcut *other = g_ptr_array_index(e->shortcuts, i);
    guint other_key = 0;
    GdkModifierType other_mods = 0;
    gtk_accelerator_parse(other->value, &other_key, &other_mods);
    if (other != shortcut && other_key == key && other_mods == mods) {
      g_autofree char *message =
          g_strdup_printf("This shortcut is assigned to %s.", other->label);
      show_error_dialog(GTK_WINDOW(e->window), "Shortcut already in use",
                        message);
      return FALSE;
    }
  }
  g_free(shortcut->value);
  shortcut->value = g_steal_pointer(&canonical);
  g_autofree char *human = accel_to_human(shortcut->value);
  gtk_label_set_text(GTK_LABEL(shortcut->shortcut), *human ? human : "None");
  if (g_strcmp0(shortcut->key, "kb_cancel") == 0) {
    g_autofree char *cancel_keys =
        *human ? g_strdup_printf("%s / esc, esc", human) : g_strdup("esc, esc");
    gtk_label_set_text(GTK_LABEL(shortcut->shortcut), cancel_keys);
  }
  gtk_widget_set_tooltip_text(shortcut->shortcut, shortcut->value);
  shortcuts_recompute_dirty(e);
  return TRUE;
}

static void shortcut_inline_changed(GtkEditable *entry, Shortcut *shortcut) {
  if (gtk_stack_get_visible_child(GTK_STACK(shortcut->stack)) ==
      GTK_WIDGET(entry)) {
    shortcuts_recompute_dirty(shortcut->editor);
  }
}

static void shortcut_inline_done(GtkEntry *entry, Shortcut *shortcut) {
  if (shortcut_assign(shortcut, gtk_editable_get_text(GTK_EDITABLE(entry))))
    gtk_stack_set_visible_child_name(GTK_STACK(shortcut->stack), "label");
}

static void shortcut_inline_start(GtkGestureClick *gesture, int presses,
                                  double x, double y, Shortcut *shortcut) {
  (void)gesture;
  (void)x;
  (void)y;
  if (presses != 2)
    return;
  gtk_editable_set_text(GTK_EDITABLE(shortcut->entry), shortcut->value);
  gtk_stack_set_visible_child_name(GTK_STACK(shortcut->stack), "entry");
  gtk_widget_grab_focus(shortcut->entry);
  gtk_editable_select_region(GTK_EDITABLE(shortcut->entry), 0, -1);
}

static gboolean shortcut_inline_key(GtkEventControllerKey *controller,
                                    guint key, guint code, GdkModifierType mods,
                                    Shortcut *shortcut) {
  (void)controller;
  (void)code;
  (void)mods;
  if (key != GDK_KEY_Escape)
    return FALSE;
  gtk_stack_set_visible_child_name(GTK_STACK(shortcut->stack), "label");
  shortcuts_recompute_dirty(shortcut->editor);
  return TRUE;
}

static gboolean shortcut_filter(GtkListBoxRow *row, gpointer data) {
  Editor *e = data;
  Shortcut *shortcut = g_object_get_data(G_OBJECT(row), "shortcut");
  if (shortcut == NULL)
    return TRUE;
  g_autofree char *query = g_utf8_casefold(
      gtk_editable_get_text(GTK_EDITABLE(e->shortcut_search)), -1);
  g_autofree char *label = g_utf8_casefold(shortcut->label, -1);
  g_autofree char *group = g_utf8_casefold(shortcut->group, -1);
  g_autofree char *value = g_utf8_casefold(shortcut->value, -1);
  g_autofree char *human =
      g_utf8_casefold(gtk_label_get_text(GTK_LABEL(shortcut->shortcut)), -1);
  return strstr(label, query) != NULL || strstr(group, query) != NULL ||
         strstr(value, query) != NULL || strstr(human, query) != NULL;
}

static void shortcut_search_changed(GtkSearchEntry *search, Editor *e) {
  (void)search;
  gtk_list_box_invalidate_filter(GTK_LIST_BOX(e->shortcut_list));
}

static void shortcut_selected(GtkListBox *list, GtkListBoxRow *row, Editor *e) {
  (void)list;
  gtk_widget_set_sensitive(e->shortcut_change, row != NULL);
}

static gboolean shortcut_capture_key(GtkEventControllerKey *controller,
                                     guint key, guint code,
                                     GdkModifierType mods, gpointer data) {
  (void)code;
  GtkWidget *entry = data;
  if (key == GDK_KEY_Escape) {
    GtkRoot *root = gtk_widget_get_root(
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
    if (GTK_IS_WINDOW(root))
      gtk_window_destroy(GTK_WINDOW(root));
    return TRUE;
  }
  if (key == GDK_KEY_Tab || key == GDK_KEY_ISO_Left_Tab ||
      (key == GDK_KEY_Return &&
       !(mods & gtk_accelerator_get_default_mod_mask())))
    return FALSE;
  if (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R ||
      key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R ||
      key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R || key == GDK_KEY_Super_L ||
      key == GDK_KEY_Super_R)
    return TRUE;
  mods &= gtk_accelerator_get_default_mod_mask();
  g_autofree char *accel = gtk_accelerator_name(gdk_keyval_to_lower(key), mods);
  gtk_editable_set_text(GTK_EDITABLE(entry), accel);
  return TRUE;
}

static void shortcut_capture_save(GtkButton *button, Shortcut *shortcut) {
  GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(button)));
  GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "shortcut-entry");
  if (shortcut_assign(shortcut, gtk_editable_get_text(GTK_EDITABLE(entry))))
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void shortcut_clear(GtkButton *button, GtkWidget *entry) {
  (void)button;
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void shortcut_change_clicked(GtkButton *button, Editor *e) {
  (void)button;
  GtkListBoxRow *row =
      gtk_list_box_get_selected_row(GTK_LIST_BOX(e->shortcut_list));
  Shortcut *shortcut =
      row != NULL ? g_object_get_data(G_OBJECT(row), "shortcut") : NULL;
  if (shortcut == NULL)
    return;
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Change shortcut");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(e->window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_window_set_child(GTK_WINDOW(dialog), box);
  GtkWidget *label = gtk_label_new(shortcut->label);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_box_append(GTK_BOX(box), label);
  GtkWidget *entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), shortcut->value);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Press a key combination");
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Shortcut", -1);
  gtk_box_append(GTK_BOX(box), entry);
  g_object_set_data(G_OBJECT(dialog), "shortcut-entry", entry);
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  gtk_event_controller_set_name(keys, "promptr-shortcut-capture");
  g_signal_connect(keys, "key-pressed", G_CALLBACK(shortcut_capture_key),
                   entry);
  gtk_widget_add_controller(entry, keys);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), buttons);
  GtkWidget *clear = gtk_button_new_with_label("Clear");
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *ok = gtk_button_new_with_label("OK");
  gtk_box_append(GTK_BOX(buttons), clear);
  gtk_box_append(GTK_BOX(buttons), cancel);
  gtk_box_append(GTK_BOX(buttons), ok);
  g_signal_connect(clear, "clicked", G_CALLBACK(shortcut_clear), entry);
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy),
                           dialog);
  g_signal_connect(ok, "clicked", G_CALLBACK(shortcut_capture_save), shortcut);
  gtk_window_set_default_widget(GTK_WINDOW(dialog), ok);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(entry);
}

static void shortcut_header(GtkListBoxRow *row, GtkListBoxRow *before,
                            gpointer data) {
  (void)data;
  Shortcut *shortcut = g_object_get_data(G_OBJECT(row), "shortcut");
  Shortcut *previous =
      before != NULL ? g_object_get_data(G_OBJECT(before), "shortcut") : NULL;
  if (shortcut != NULL &&
      (previous == NULL || g_strcmp0(shortcut->group, previous->group) != 0)) {
    GtkWidget *label = gtk_label_new(shortcut->group);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 3);
    gtk_list_box_row_set_header(row, label);
  } else
    gtk_list_box_row_set_header(row, NULL);
}

static GtkWidget *shortcuts_page(Editor *e) {
  gsize binding_count;
  const ShortcutDefinition *bindings = shortcut_definitions(&binding_count);
  e->shortcuts = g_ptr_array_new_with_free_func(shortcut_free);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  e->shortcut_search = gtk_search_entry_new();
  gtk_widget_set_tooltip_text(e->shortcut_search,
                              "Filter actions and shortcuts");
  gtk_box_append(GTK_BOX(box), e->shortcut_search);
  GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *action = gtk_label_new("Action");
  gtk_label_set_xalign(GTK_LABEL(action), 0);
  gtk_widget_set_hexpand(action, TRUE);
  gtk_widget_add_css_class(action, "heading");
  gtk_box_append(GTK_BOX(columns), action);
  GtkWidget *key = gtk_label_new("Shortcut");
  gtk_label_set_xalign(GTK_LABEL(key), 0);
  gtk_widget_set_size_request(key, 220, -1);
  gtk_widget_add_css_class(key, "heading");
  gtk_box_append(GTK_BOX(columns), key);
  gtk_box_append(GTK_BOX(box), columns);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_widget_add_css_class(scroll, "frame");
  gtk_box_append(GTK_BOX(box), scroll);
  e->shortcut_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(e->shortcut_list),
                                  GTK_SELECTION_SINGLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), e->shortcut_list);
  for (guint i = 0; i < binding_count; i++) {
    Shortcut *shortcut = g_new0(Shortcut, 1);
    shortcut->editor = e;
    shortcut->group = bindings[i].group;
    shortcut->label = bindings[i].label;
    shortcut->key = bindings[i].key;
    shortcut->fallback = bindings[i].fallback;
    shortcut->value = runtime_config_get_string(e->win->config, shortcut->key,
                                                shortcut->fallback);
    g_ptr_array_add(e->shortcuts, shortcut);
    GtkWidget *row = gtk_list_box_row_new();
    shortcut->row = row;
    g_object_set_data(G_OBJECT(row), "shortcut", shortcut);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(content, 8);
    gtk_widget_set_margin_end(content, 8);
    gtk_widget_set_margin_top(content, 3);
    gtk_widget_set_margin_bottom(content, 3);
    GtkWidget *label = gtk_label_new(shortcut->label);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(content), label);
    shortcut->stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(shortcut->stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(shortcut->stack), FALSE);
    gtk_widget_set_size_request(shortcut->stack, 212, -1);
    g_autofree char *human = accel_to_human(shortcut->value);
    shortcut->shortcut = gtk_label_new(*human ? human : "None");
    if (g_strcmp0(shortcut->key, "kb_cancel") == 0 && !*human)
      gtk_label_set_text(GTK_LABEL(shortcut->shortcut), "esc, esc");
    gtk_label_set_xalign(GTK_LABEL(shortcut->shortcut), 0);
    gtk_stack_add_named(GTK_STACK(shortcut->stack), shortcut->shortcut,
                        "label");
    shortcut->entry = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(shortcut->entry), 14);
    gtk_entry_set_placeholder_text(GTK_ENTRY(shortcut->entry), "<Control>k");
    gtk_stack_add_named(GTK_STACK(shortcut->stack), shortcut->entry, "entry");
    g_signal_connect(shortcut->entry, "changed",
                     G_CALLBACK(shortcut_inline_changed), shortcut);
    g_signal_connect(shortcut->entry, "activate",
                     G_CALLBACK(shortcut_inline_done), shortcut);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(shortcut_inline_key),
                     shortcut);
    gtk_widget_add_controller(shortcut->entry, keys);
    gtk_box_append(GTK_BOX(content), shortcut->stack);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click),
                                  "promptr-shortcut-inline");
    g_signal_connect(click, "pressed", G_CALLBACK(shortcut_inline_start),
                     shortcut);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_list_box_append(GTK_LIST_BOX(e->shortcut_list), row);
  }
  e->shortcut_change = gtk_button_new_with_label("Change");
  gtk_widget_set_halign(e->shortcut_change, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(e->shortcut_change, 240, -1);
  gtk_widget_set_sensitive(e->shortcut_change, FALSE);
  gtk_box_append(GTK_BOX(box), e->shortcut_change);
  gtk_list_box_set_header_func(GTK_LIST_BOX(e->shortcut_list), shortcut_header,
                               NULL, NULL);
  gtk_list_box_set_filter_func(GTK_LIST_BOX(e->shortcut_list), shortcut_filter,
                               e, NULL);
  g_signal_connect(e->shortcut_search, "search-changed",
                   G_CALLBACK(shortcut_search_changed), e);
  g_signal_connect(e->shortcut_list, "row-selected",
                   G_CALLBACK(shortcut_selected), e);
  g_signal_connect(e->shortcut_change, "clicked",
                   G_CALLBACK(shortcut_change_clicked), e);
  return box;
}

void provider_settings_show_profile(AppWindow *win, const char *profile) {
  GListModel *windows = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    g_autoptr(GtkWindow) window = g_list_model_get_item(windows, i);
    Editor *existing = g_object_get_data(G_OBJECT(window), "provider-editor");
    if (existing != NULL && existing->win == win && !existing->closed) {
      if (profile != NULL && !existing->busy &&
          g_strcmp0(profile, existing->profile->id) != 0) {
        if (existing->dirty) {
          set_status(existing,
                     "Apply or discard changes before switching providers.",
                     FALSE);
          gtk_window_present(app_window_dialog_parent(window));
          return;
        }
        ProviderProfile *selected_profile =
            provider_profile_load(profile, NULL);
        if (selected_profile != NULL) {
          provider_profile_free(existing->profile);
          existing->profile = selected_profile;
          rebuild_profiles(existing);
          editor_fill(existing);
        }
      }
      if (profile != NULL)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(existing->sections), 2);
      gtk_window_present(app_window_dialog_parent(window));
      return;
    }
  }
  Editor *e = g_new0(Editor, 1);
  e->win = win;
  e->provider_labels = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
  e->inputs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  e->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(e->window), "Preferences");
  gtk_window_set_modal(GTK_WINDOW(e->window), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(e->window), GTK_WINDOW(win->window));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(e->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(e->window), 820, 560);
  g_object_set_data_full(G_OBJECT(e->window), "provider-editor", e,
                         editor_free);
  g_signal_connect(e->window, "close-request", G_CALLBACK(close_editor), e);
  g_signal_connect(e->window, "notify::visible", G_CALLBACK(visibility_changed),
                   e);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(preferences_key_pressed), e);
  gtk_widget_add_controller(e->window, keys);
  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  e->sections = gtk_notebook_new();
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(root, 6);
  gtk_widget_set_margin_end(root, 6);
  gtk_widget_set_margin_top(root, 6);
  gtk_widget_set_margin_bottom(root, 6);
  gtk_window_set_child(GTK_WINDOW(e->window), root);
  gtk_widget_set_vexpand(e->sections, TRUE);
  gtk_notebook_set_tab_pos(GTK_NOTEBOOK(e->sections), GTK_POS_LEFT);
  gtk_box_append(GTK_BOX(root), e->sections);
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(root), actions);
  e->save = gtk_button_new_with_label("Apply");
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  e->ok = gtk_button_new_with_label("OK");
  GtkWidget *action_buttons[] = {e->save, cancel, e->ok};
  for (guint i = 0; i < G_N_ELEMENTS(action_buttons); i++) {
    gtk_widget_set_size_request(action_buttons[i], 84, -1);
    gtk_box_append(GTK_BOX(actions), action_buttons[i]);
  }
  gtk_window_set_default_widget(GTK_WINDOW(e->window), e->ok);
  g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_preferences), e);
  g_signal_connect(e->ok, "clicked", G_CALLBACK(ok_preferences), e);
  gtk_notebook_append_page(GTK_NOTEBOOK(e->sections), editor_page(e),
                           gtk_label_new("Editor"));
  gtk_notebook_append_page(GTK_NOTEBOOK(e->sections), shortcuts_page(e),
                           gtk_label_new("Keybindings"));
  gtk_notebook_append_page(GTK_NOTEBOOK(e->sections), body,
                           gtk_label_new("Providers"));
  if (profile != NULL)
    gtk_notebook_set_current_page(GTK_NOTEBOOK(e->sections), 2);
  GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request(sidebar, 155, -1);
  gtk_widget_set_margin_start(sidebar, 12);
  gtk_widget_set_margin_end(sidebar, 12);
  gtk_widget_set_margin_top(sidebar, 12);
  gtk_widget_set_margin_bottom(sidebar, 12);
  gtk_box_append(GTK_BOX(body), sidebar);
  GtkWidget *profile_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(profile_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(profile_scroll, TRUE);
  gtk_box_append(GTK_BOX(sidebar), profile_scroll);
  e->profiles = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(e->profiles),
                                  GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(e->profiles, "navigation-sidebar");
  gtk_accessible_update_property(GTK_ACCESSIBLE(e->profiles),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Provider profiles", -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(profile_scroll),
                                e->profiles);
  e->add = gtk_button_new_with_label("Add provider");
  e->remove = gtk_button_new_with_label("Remove provider");
  gtk_widget_add_css_class(e->remove, "destructive-action");
  gtk_box_append(GTK_BOX(sidebar), e->add);
  gtk_box_append(GTK_BOX(sidebar), e->remove);
  gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(detail, TRUE);
  gtk_box_append(GTK_BOX(body), detail);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(detail), scroll);
  e->form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(e->form, 12);
  gtk_widget_set_margin_end(e->form, 12);
  gtk_widget_set_margin_top(e->form, 12);
  gtk_widget_set_margin_bottom(e->form, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), e->form);
  GtkWidget *title = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  e->heading = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(e->heading), 0);

  gtk_widget_add_css_class(e->heading, "heading");
  gtk_label_set_ellipsize(GTK_LABEL(e->heading), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(e->heading), 38);
  gtk_box_append(GTK_BOX(title), e->heading);
  gtk_box_append(GTK_BOX(e->form), title);
  GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append(GTK_BOX(e->form), identity);
  e->plugin_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(identity), e->plugin_row);
  GtkWidget *plugin_label = gtk_label_new("Provider type");
  gtk_label_set_xalign(GTK_LABEL(plugin_label), 0);
  gtk_size_group_add_widget(e->provider_labels, plugin_label);
  gtk_box_append(GTK_BOX(e->plugin_row), plugin_label);
  GtkStringList *plugins = gtk_string_list_new(plugin_manager_get_providers());
  gtk_string_list_append(plugins, "custom");
  e->plugins = gtk_drop_down_new(G_LIST_MODEL(plugins), NULL);
  gtk_widget_set_hexpand(e->plugins, TRUE);
  gtk_label_set_mnemonic_widget(GTK_LABEL(plugin_label), e->plugins);
  gtk_box_append(GTK_BOX(e->plugin_row), e->plugins);
  e->name = entry_row(e, identity, "Profile name");
  gtk_box_append(GTK_BOX(e->form),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *connection = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append(GTK_BOX(e->form), connection);
  GtkWidget *connection_title = gtk_label_new("Connection");
  gtk_label_set_xalign(GTK_LABEL(connection_title), 0);
  gtk_widget_add_css_class(connection_title, "heading");
  gtk_box_append(GTK_BOX(connection), connection_title);
  e->fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append(GTK_BOX(connection), e->fields);
  GtkWidget *model_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(e->form), model_group);
  GtkWidget *model_title = gtk_label_new("Default model");
  gtk_label_set_xalign(GTK_LABEL(model_title), 0);
  gtk_size_group_add_widget(e->provider_labels, model_title);
  gtk_box_append(GTK_BOX(model_group), model_title);
  GtkWidget *model_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(model_row, TRUE);
  e->model_choices = gtk_drop_down_new(NULL, NULL);
  g_autoptr(GtkExpression) model_expression =
      gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
  gtk_drop_down_set_expression(GTK_DROP_DOWN(e->model_choices),
                               model_expression);
  gtk_drop_down_set_enable_search(GTK_DROP_DOWN(e->model_choices), TRUE);
#if GTK_CHECK_VERSION(4, 12, 0)
  gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(e->model_choices),
                                      GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
#endif
  g_autoptr(GtkListItemFactory) model_factory =
      gtk_signal_list_item_factory_new();
  g_signal_connect(model_factory, "setup", G_CALLBACK(model_item_setup), NULL);
  g_signal_connect(model_factory, "bind", G_CALLBACK(model_item_bind), NULL);
  gtk_drop_down_set_factory(GTK_DROP_DOWN(e->model_choices), model_factory);
  gtk_widget_set_hexpand(e->model_choices, TRUE);
  gtk_label_set_mnemonic_widget(GTK_LABEL(model_title), e->model_choices);
  e->discover = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(e->discover, "Refresh available models");
  gtk_accessible_update_property(GTK_ACCESSIBLE(e->discover),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Refresh available models", -1);
  gtk_box_append(GTK_BOX(model_row), e->model_choices);
  gtk_box_append(GTK_BOX(model_row), e->discover);
  gtk_box_append(GTK_BOX(model_group), model_row);
  g_signal_connect(e->model_choices, "notify::selected",
                   G_CALLBACK(model_choice_changed), e);
  e->advanced = gtk_expander_new("Advanced");
  gtk_box_append(GTK_BOX(e->form), e->advanced);
  GtkWidget *advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(advanced, 12);
  gtk_expander_set_child(GTK_EXPANDER(e->advanced), advanced);
  e->command = entry_row(e, advanced, "Plugin executable");
  e->arguments = entry_row(e, advanced, "Arguments");
  gtk_widget_set_tooltip_text(e->arguments,
                              "Quote arguments containing spaces");
  e->model = entry_row(e, advanced, "Model ID");
  gtk_entry_set_placeholder_text(GTK_ENTRY(e->model), "Use backend default");
  e->advanced_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append(GTK_BOX(advanced), e->advanced_fields);
  gtk_box_append(GTK_BOX(detail),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(footer, 12);
  gtk_widget_set_margin_end(footer, 12);
  gtk_widget_set_margin_top(footer, 12);
  gtk_widget_set_margin_bottom(footer, 12);
  gtk_box_append(GTK_BOX(detail), footer);
  GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  e->spinner = gtk_spinner_new();
  gtk_box_append(GTK_BOX(progress), e->spinner);
  e->status = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(e->status), TRUE);
  gtk_label_set_lines(GTK_LABEL(e->status), 3);
  gtk_label_set_max_width_chars(GTK_LABEL(e->status), 55);
  gtk_label_set_ellipsize(GTK_LABEL(e->status), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(e->status), 0);
  gtk_widget_set_hexpand(e->status, TRUE);
  gtk_box_append(GTK_BOX(progress), e->status);
  gtk_box_append(GTK_BOX(footer), progress);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(footer), buttons);
  e->revert = gtk_button_new_with_label("Discard changes");
  gtk_box_append(GTK_BOX(buttons), e->revert);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(buttons), spacer);
  e->stop = gtk_button_new_with_label("Stop test");
  e->test = gtk_button_new_with_label("Test connection");
  gtk_widget_set_tooltip_text(
      e->test, "Send a small test request; usage charges may apply");

  gtk_box_append(GTK_BOX(buttons), e->stop);
  gtk_box_append(GTK_BOX(buttons), e->test);

  g_signal_connect(e->add, "clicked", G_CALLBACK(add_clicked), e);
  g_signal_connect(e->revert, "clicked", G_CALLBACK(revert_clicked), e);
  g_signal_connect(e->stop, "clicked", G_CALLBACK(stop_clicked), e);
  GtkWidget *entries[] = {e->command, e->arguments, e->model};
  g_signal_connect(e->name, "changed", G_CALLBACK(name_changed), e);
  for (guint i = 0; i < G_N_ELEMENTS(entries); i++)
    g_signal_connect(entries[i], "changed", G_CALLBACK(field_changed), e);
  g_signal_connect(e->profiles, "row-selected", G_CALLBACK(profile_changed), e);
  g_signal_connect(e->plugins, "notify::selected", G_CALLBACK(plugin_changed),
                   e);
  g_signal_connect(e->test, "clicked", G_CALLBACK(test_clicked), e);
  g_signal_connect(e->discover, "clicked", G_CALLBACK(discover_clicked), e);
  g_signal_connect(e->save, "clicked", G_CALLBACK(save_clicked), e);
  g_signal_connect(e->remove, "clicked", G_CALLBACK(delete_clicked), e);
  Tab *active = app_window_get_active_tab(win);
  e->profile = provider_profile_load(profile != NULL  ? profile
                                     : active != NULL ? active->provider_name
                                                      : NULL,
                                     NULL);
  if (e->profile == NULL) {
    g_autofree char *plugin = selected(e->plugins);
    e->profile = provider_profile_new(plugin);
  }
  rebuild_profiles(e);
  editor_fill(e);
  gtk_window_present(GTK_WINDOW(e->window));
}

void provider_settings_show(AppWindow *win) {
  provider_settings_show_profile(win, NULL);
}
