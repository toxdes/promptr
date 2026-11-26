#include "window.h"
#include "agent-context.h"
#include "app-menu.h"
#include "app-style.h"
#include "config.h"
#include "configfile.h"
#include "plugin-runner.h"
#include "provider-manager.h"
#include "provider-profile.h"
#include "provider-settings.h"
#include "shortcuts.h"

#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  guint refs;
  guint pending;
  AppWindow *win;
} ModelOwner;

static void model_owner_unref(ModelOwner *owner) {
  if (--owner->refs == 0)
    g_free(owner);
}

static void hydrate_tab(Tab *tab);
static void refresh_history(Tab *tab, gboolean older);
static void on_load_older(GtkButton *button, Tab *tab);
static void on_choose_project(GtkButton *button, Tab *tab);
static void clear_project(GtkButton *button, Tab *tab);
static void latest_history(GtkButton *button, Tab *tab);
static void on_choose_agent(GtkButton *button, Tab *tab);
static void on_manual_model(GtkButton *button, Tab *tab);
static void on_manage_providers(AppWindow *win);
static void on_context_action(GSimpleAction *action, GVariant *param,
                              gpointer data);
static void show_provider_error(Tab *tab, const char *title, const char *detail,
                                const char *profile);
static void on_history_list(AppWindow *win);
static void session_write(AppWindow *win);
static Tab *session_restore(AppWindow *win);
static void on_submit(AppWindow *win);
static void on_cancel(AppWindow *win);
static void on_copy(AppWindow *win);
static void on_close(AppWindow *win);
static void on_quit(AppWindow *win);
static void on_follow_up_toggled(Tab *tab);
static void on_provider_event(gpointer source, const ProviderEvent *event,
                              gpointer user_data);
static void on_set_provider(GSimpleAction *action, GVariant *state,
                            gpointer user_data);
static void rebuild_provider_menu(GMenu *submenu, Tab *tab);
static char **dropdown_values(GtkWidget *dropdown);
static void rebuild_agent_menu(AppWindow *win, Tab *tab);
static void rebuild_model_menu(AppWindow *win, Tab *tab);
static void cancel_tab(Tab *tab);
static void update_submit_sensitivity(Tab *tab);
static void update_picker_sensitivity(Tab *tab);
static char *get_trimmed_text(GtkWidget *text_view);
static void set_prompt_focused(Tab *tab);

static void set_loading_state(Tab *tab, const char *cmd);

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win);
static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win);
static void on_prompt_focus_changed(GObject *object, GParamSpec *pspec,
                                    gpointer user_data);
static gboolean esc_arm_timeout_cb(gpointer data);
static void disarm_escape(AppWindow *win);
static gboolean status_pop_cb(gpointer data);
static gboolean on_close_request(GtkWindow *window, gpointer user_data);
static void on_prompt_changed(GtkTextBuffer *buffer, Tab *tab);
static void on_dropdown_changed(GObject *self, GParamSpec *pspec, Tab *tab);
static void update_cmd_preview(Tab *tab);
static void on_log(AppWindow *win);
static void on_log_close(AppWindow *win);
static void on_shortcuts(AppWindow *win);
static void on_shortcuts_close(AppWindow *win);
static void on_about(AppWindow *win);

static void on_menu_toggle_bar(GSimpleAction *action, GVariant *state,
                               gpointer user_data);
static void on_menu_close_tab(GSimpleAction *action, GVariant *param,
                              gpointer user_data);
static void on_menu_toggle_follow_up(GSimpleAction *action, GVariant *param,
                                     gpointer user_data);
static void on_menu_toggle_layout(GSimpleAction *action, GVariant *param,
                                  gpointer user_data);
static void on_menu_popout(GSimpleAction *action, GVariant *param,
                           gpointer user_data);
static void on_menu_toggle_status_bar(GSimpleAction *action, GVariant *state,
                                      gpointer user_data);
static void on_menu_cycle_agent_next(GSimpleAction *action, GVariant *param,
                                     gpointer user_data);
static void on_menu_cycle_agent_prev(GSimpleAction *action, GVariant *param,
                                     gpointer user_data);
static void on_menu_cycle_model_next(GSimpleAction *action, GVariant *param,
                                     gpointer user_data);
static void on_menu_cycle_model_prev(GSimpleAction *action, GVariant *param,
                                     gpointer user_data);
static void on_menu_cycle_agent_set(GSimpleAction *action, GVariant *param,
                                    gpointer user_data);
static void on_menu_cycle_model_set(GSimpleAction *action, GVariant *param,
                                    gpointer user_data);

/* Fwd: layout helpers */
static GtkWidget *create_prompt_section(Tab *tab, AppWindow *win);
static GtkWidget *create_follow_up_row(Tab *tab, AppWindow *win);
static GtkWidget *create_agent_row(Tab *tab, AppWindow *win);
static GtkWidget *create_output_section(Tab *tab, AppWindow *win);
static GtkWidget *create_marked_row(Tab *tab, AppWindow *win);
static GtkWidget *create_action_row(Tab *tab, AppWindow *win);
static GtkWidget *create_status_bar(AppWindow *win);
static void setup_tooltips(Tab *tab, AppWindow *win);
static void apply_layout(Tab *tab);
static void toggle_popout(Tab *tab);
static void close_popups(AppWindow *win);
static void cycle_dropdown_next(GtkWidget *dropdown);
static void cycle_dropdown_prev(GtkWidget *dropdown);
static void update_window_title(AppWindow *win, const char *tab_name);
static void add_dropdown_hover(GtkWidget *dropdown, AppWindow *win);
static void set_paned_half(GtkPaned *paned, Tab *tab);
static void set_paned_half_notify(GObject *object, GParamSpec *pspec,
                                  gpointer data);
static void on_paned_position_changed(GObject *object, GParamSpec *pspec,
                                      gpointer data);

static void set_paned_half(GtkPaned *paned, Tab *tab) {
  int size = tab->layout_mode == 0 ? gtk_widget_get_height(GTK_WIDGET(paned))
                                   : gtk_widget_get_width(GTK_WIDGET(paned));
  if (size > 0 && !tab->paned_positioned) {
    gtk_paned_set_position(paned, size / 2);
    tab->paned_positioned = TRUE;
  }
}

static void set_paned_half_notify(GObject *object, GParamSpec *pspec,
                                  gpointer data) {
  (void)pspec;
  set_paned_half(GTK_PANED(object), data);
}

#define AGENT_ROW_LABEL_THRESHOLD 280

static void on_paned_position_changed(GObject *object, GParamSpec *pspec,
                                      gpointer data) {
  Tab *tab = data;
  GtkPaned *paned;
  gboolean narrow;

  (void)pspec;
  if (tab->layout_mode != 1)
    return;
  paned = GTK_PANED(object);
  narrow = gtk_paned_get_position(paned) < AGENT_ROW_LABEL_THRESHOLD;
  if (tab->agent_text_label != NULL)
    gtk_widget_set_visible(tab->agent_text_label, !narrow);
  if (tab->model_text_label != NULL)
    gtk_widget_set_visible(tab->model_text_label, !narrow);
}

static void dropdown_trunc_factory_setup(GtkSignalListItemFactory *factory,
                                         GtkListItem *item, gpointer data) {
  GtkWidget *label;

  (void)factory;
  (void)data;
  label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_width_chars(GTK_LABEL(label), 1);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 25);
  gtk_list_item_set_child(item, label);
}

static void dropdown_trunc_factory_bind(GtkSignalListItemFactory *factory,
                                        GtkListItem *item, gpointer data) {
  GtkWidget *label;
  GtkStringObject *obj;

  (void)factory;
  (void)data;
  label = gtk_list_item_get_child(item);
  obj = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
  gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(obj));
}

static void dropdown_full_factory_setup(GtkSignalListItemFactory *factory,
                                        GtkListItem *item, gpointer data) {
  GtkWidget *label;

  (void)factory;
  (void)data;
  label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_list_item_set_child(item, label);
}

static void dropdown_full_factory_bind(GtkSignalListItemFactory *factory,
                                       GtkListItem *item, gpointer data) {
  GtkWidget *label;
  GtkStringObject *obj;

  (void)factory;
  (void)data;
  label = gtk_list_item_get_child(item);
  obj = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
  gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(obj));
}
struct PopupEscCtx {
  gpointer data;
  void (*close_fn)(gpointer);
};

static gboolean on_popup_esc(GtkEventControllerKey *ctrl, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer data) {
  struct PopupEscCtx *ctx = data;

  (void)ctrl;
  (void)keycode;
  (void)state;
  if (keyval == GDK_KEY_Escape) {
    ctx->close_fn(ctx->data);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void esc_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}
static void set_status_text(AppWindow *win, const char *text);
void log_append(AppWindow *win, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static gboolean hex_to_rgba(const char *hex, GdkRGBA *out);
static void on_gutter_click(GtkGestureClick *gesture, int n_press, double x,
                            double y, AppWindow *win);
static void update_marked_label(Tab *tab);
static void apply_default_marks(Tab *tab, GtkTextBuffer *buf);
static char *get_marked_text(Tab *tab);
static void status_bar_on_hover(GtkWidget *widget, AppWindow *win,
                                const char *text);
static GtkWidget *kbd_label(const char *text);
static GtkWidget *desc_label(const char *text);
static GtkWidget *cell_box(GtkWidget *child, const char *row_class,
                           gboolean last_col);

/* ── layout helpers ─────────────────────────────────────────────── */

static GtkWidget *create_prompt_section(Tab *tab, AppWindow *win) {
  GtkWidget *box, *scroll, *overlay, *label, *hdr;
  g_autofree char *prompt_text = NULL;
  g_autofree char *kb_human = NULL;

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(hdr, TRUE);
  gtk_widget_set_margin_bottom(hdr, 4);

  {
    g_autofree char *kb_focus_str = NULL;

    kb_focus_str = runtime_config_get_string(win->config, "kb_focus_prompt",
                                             KB_FOCUS_PROMPT);
    kb_human = accel_to_human(kb_focus_str);
  }
  prompt_text = g_markup_printf_escaped(
      "Prompt  <span size='x-small' foreground='#888'>%s to focus</span>",
      kb_human);
  label = gtk_label_new(prompt_text);
  gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_margin_bottom(label, 4);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(hdr), label);

  {
    GtkWidget *btns;

    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    gtk_widget_set_valign(btns, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hdr), btns);
    tab->prompt_btns = btns;
  }

  gtk_box_append(GTK_BOX(box), hdr);

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),
                                                   TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);

  tab->prompt_view = GTK_WIDGET(gtk_source_view_new());
  g_object_set_data(G_OBJECT(tab->prompt_view), "focus-hint", label);
  gtk_source_view_set_show_line_numbers(
      GTK_SOURCE_VIEW(tab->prompt_view),
      runtime_config_get_bool(win->config, "editor_line_numbers",
                              EDITOR_LINE_NUMBERS_DEFAULT));
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->prompt_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->prompt_view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tab->prompt_view), 4);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tab->prompt_view), FALSE);
  gtk_widget_add_css_class(tab->prompt_view, "prompt-font");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tab->prompt_view);

  {
    GtkWidget *plabel;

    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);

    plabel =
        gtk_label_new("E.g. list all files in current dir, except .md files");
    gtk_widget_set_halign(plabel, GTK_ALIGN_START);
    gtk_widget_set_valign(plabel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(plabel, 56);
    gtk_widget_set_margin_top(plabel, 6);
    gtk_widget_set_opacity(plabel, 0.5);
    gtk_widget_add_css_class(plabel, "dim-label");
    gtk_widget_add_css_class(plabel, "prompt-font");
    gtk_widget_set_can_target(plabel, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), plabel);
    tab->placeholder_label = plabel;
  }

  gtk_box_append(GTK_BOX(box), overlay);

  {
    GtkTextBuffer *buffer;
    GtkEventController *key_ctrl;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
    g_signal_connect(buffer, "changed", G_CALLBACK(on_prompt_changed), tab);

    key_ctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(tab->prompt_view, key_ctrl);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_prompt_key_pressed),
                     win);

    g_signal_connect(tab->prompt_view, "notify::has-focus",
                     G_CALLBACK(on_prompt_focus_changed), win);
  }

  return box;
}

static GtkWidget *create_follow_up_row(Tab *tab, AppWindow *win) {
  GtkWidget *furow;

  (void)win;

  furow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_top(furow, 4);
  gtk_widget_set_margin_bottom(furow, 4);

  tab->follow_up_check =
      gtk_check_button_new_with_label("This prompt is a follow up");
  gtk_widget_set_sensitive(tab->follow_up_check, FALSE);
  g_signal_connect_swapped(tab->follow_up_check, "toggled",
                           G_CALLBACK(on_follow_up_toggled), tab);
  gtk_box_append(GTK_BOX(furow), tab->follow_up_check);

  return furow;
}

static GtkWidget *create_agent_row(Tab *tab, AppWindow *win) {
  GtkWidget *row, *label, *filler;
  GtkStringList *list;
  char **opts = NULL;
  int i;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  label = gtk_label_new("Agent");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_end(label, 4);
  gtk_box_append(GTK_BOX(row), label);
  tab->agent_text_label = label;

  opts = agent_context_list();
  list = gtk_string_list_new(NULL);
  for (i = 0; opts[i] != NULL && opts[i][0] != '\0'; i++) {
    gtk_string_list_append(list, opts[i]);
  }
  tab->agent_dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(tab->agent_dropdown), 0);
  gtk_widget_set_sensitive(tab->agent_dropdown,
                           i > 1 ||
                               (i == 1 && g_strcmp0(opts[0], "None") != 0));
  gtk_widget_set_hexpand(tab->agent_dropdown, TRUE);
  {
    GtkListItemFactory *f;

    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(dropdown_trunc_factory_setup),
                     NULL);
    g_signal_connect(f, "bind", G_CALLBACK(dropdown_trunc_factory_bind), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(tab->agent_dropdown), f);

    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(dropdown_full_factory_setup), NULL);
    g_signal_connect(f, "bind", G_CALLBACK(dropdown_full_factory_bind), NULL);
    gtk_drop_down_set_list_factory(GTK_DROP_DOWN(tab->agent_dropdown), f);
  }
  g_signal_connect(tab->agent_dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), tab);
  gtk_box_append(GTK_BOX(row), tab->agent_dropdown);
  g_strfreev(opts);
  opts = NULL;

  label = gtk_label_new("Model");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_end(label, 4);
  gtk_box_append(GTK_BOX(row), label);
  tab->model_text_label = label;

  g_autoptr(ProviderProfile) profile =
      provider_profile_load(tab->provider_name, NULL);
  opts = profile != NULL && profile->models != NULL
             ? g_strdupv(profile->models)
             : g_strsplit("None", ",", -1);
  list = gtk_string_list_new(NULL);
  for (i = 0; opts[i] != NULL && opts[i][0] != '\0'; i++) {
    gtk_string_list_append(list, opts[i]);
  }
  tab->model_dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
  g_autoptr(GtkExpression) model_expression =
      gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
  gtk_drop_down_set_expression(GTK_DROP_DOWN(tab->model_dropdown),
                               model_expression);
  gtk_drop_down_set_enable_search(GTK_DROP_DOWN(tab->model_dropdown), TRUE);
#if GTK_CHECK_VERSION(4, 12, 0)
  gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(tab->model_dropdown),
                                      GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
#endif
  gtk_drop_down_set_selected(GTK_DROP_DOWN(tab->model_dropdown), 0);
  gtk_widget_set_sensitive(tab->model_dropdown, FALSE);
  gtk_widget_set_hexpand(tab->model_dropdown, TRUE);
  {
    GtkListItemFactory *f;

    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(dropdown_trunc_factory_setup),
                     NULL);
    g_signal_connect(f, "bind", G_CALLBACK(dropdown_trunc_factory_bind), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(tab->model_dropdown), f);

    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(dropdown_full_factory_setup), NULL);
    g_signal_connect(f, "bind", G_CALLBACK(dropdown_full_factory_bind), NULL);
    gtk_drop_down_set_list_factory(GTK_DROP_DOWN(tab->model_dropdown), f);
  }
  g_signal_connect(tab->model_dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), tab);
  gtk_box_append(GTK_BOX(row), tab->model_dropdown);
  g_strfreev(opts);
  opts = NULL;

  tab->submit_btn = gtk_button_new_with_label("Submit");
  gtk_widget_set_margin_start(tab->submit_btn, 8);
  gtk_widget_add_css_class(tab->submit_btn, "suggested-action");
  gtk_widget_set_sensitive(tab->submit_btn, FALSE);
  g_signal_connect_swapped(tab->submit_btn, "clicked", G_CALLBACK(on_submit),
                           win);
  gtk_box_append(GTK_BOX(row), tab->submit_btn);

  tab->spinner = gtk_spinner_new();
  gtk_widget_set_visible(tab->spinner, FALSE);
  gtk_widget_set_margin_start(tab->spinner, 8);
  gtk_widget_set_valign(tab->spinner, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), tab->spinner);

  tab->cancel_btn = gtk_button_new_with_label("Cancel");
  gtk_widget_set_valign(tab->cancel_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(tab->cancel_btn, 8);
  gtk_widget_set_visible(tab->cancel_btn, FALSE);
  g_signal_connect_swapped(tab->cancel_btn, "clicked", G_CALLBACK(on_cancel),
                           win);
  gtk_box_append(GTK_BOX(row), tab->cancel_btn);

  filler = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(filler, TRUE);
  gtk_box_append(GTK_BOX(row), filler);

  {
    GtkWidget *btns;

    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_box_append(GTK_BOX(row), btns);
    tab->agent_btns = btns;
  }

  return row;
}

static GtkWidget *create_output_section(Tab *tab, AppWindow *win) {
  GtkWidget *box, *scroll, *popout_btn;
  GdkRGBA c = {0};
  g_autofree char *color;

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  {
    GtkWidget *hdr;

    hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(hdr, TRUE);

    tab->output_label = gtk_label_new("Output");
    gtk_label_set_xalign(GTK_LABEL(tab->output_label), 0.0f);
    gtk_widget_set_margin_top(tab->output_label, 8);
    gtk_widget_set_margin_bottom(tab->output_label, 4);
    gtk_widget_set_hexpand(tab->output_label, TRUE);
    gtk_box_append(GTK_BOX(hdr), tab->output_label);

    popout_btn = gtk_button_new_with_label("Undock...");
    g_autofree char *popout_key = shortcut_human(win->config, "kb_popout");
    g_autofree char *popout_tip =
        *popout_key ? g_strdup_printf("Undock output (%s)", popout_key)
                    : g_strdup("Undock output");
    gtk_widget_set_tooltip_text(popout_btn, popout_tip);
    gtk_widget_set_halign(popout_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(popout_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(popout_btn, "clicked", G_CALLBACK(toggle_popout),
                             tab);
    gtk_box_append(GTK_BOX(hdr), popout_btn);
    tab->popout_btn = popout_btn;

    gtk_box_append(GTK_BOX(box), hdr);
  }

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 60);
  gtk_widget_set_vexpand(scroll, TRUE);

  tab->output_view = GTK_WIDGET(gtk_source_view_new());
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->output_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->output_view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->output_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->output_view), 2);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tab->output_view), 4);
  gtk_source_view_set_show_line_numbers(
      GTK_SOURCE_VIEW(tab->output_view),
      runtime_config_get_bool(win->config, "editor_line_numbers",
                              EDITOR_LINE_NUMBERS_DEFAULT));
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tab->output_view), FALSE);
  gtk_widget_add_css_class(tab->output_view, "output-font");
  gtk_source_view_set_show_line_marks(GTK_SOURCE_VIEW(tab->output_view), TRUE);

  color =
      runtime_config_get_string(win->config, "mark_bg_color", MARK_BG_COLOR);
  hex_to_rgba(color, &c);
  {
    GtkSourceMarkAttributes *attrs;

    attrs = gtk_source_mark_attributes_new();
    gtk_source_mark_attributes_set_background(attrs, &c);
    gtk_source_mark_attributes_set_icon_name(attrs, "media-record-symbolic");
    gtk_source_view_set_mark_attributes(GTK_SOURCE_VIEW(tab->output_view),
                                        "promptr-mark", attrs, 0);
  }

  {
    GtkGesture *gesture;

    gesture = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                               GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(tab->output_view, GTK_EVENT_CONTROLLER(gesture));
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_gutter_click), win);
  }

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tab->output_view);
  tab->output_scroll = scroll;
  gtk_box_append(GTK_BOX(box), scroll);

  return box;
}

static GtkWidget *create_marked_row(Tab *tab, AppWindow *win) {
  GtkWidget *row;

  (void)win;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);

  tab->marked_label = gtk_label_new("Marked lines: none");
  gtk_widget_set_margin_start(tab->marked_label, 48);
  gtk_label_set_xalign(GTK_LABEL(tab->marked_label), 0.0f);
  gtk_widget_set_margin_top(tab->marked_label, 4);
  gtk_widget_set_margin_bottom(tab->marked_label, 4);
  gtk_box_append(GTK_BOX(row), tab->marked_label);

  return row;
}

static GtkWidget *create_action_row(Tab *tab, AppWindow *win) {
  GtkWidget *row;

  (void)win;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  tab->copy_btn = gtk_button_new_with_label("Copy Marked Lines");
  g_object_ref_sink(tab->copy_btn);
  gtk_widget_set_sensitive(tab->copy_btn, FALSE);
  g_signal_connect_swapped(tab->copy_btn, "clicked", G_CALLBACK(on_copy), win);
  gtk_box_append(GTK_BOX(row), tab->copy_btn);

  return row;
}

/* ── tab management ──────────────────────────────────────────────── */

static void tab_switch_to(AppWindow *win, int idx);
static void close_tab(AppWindow *win, int idx);
static void on_new_tab_clicked(AppWindow *win);
GtkWidget *tab_create_widgets(Tab *tab, AppWindow *win) {
  GtkWidget *page;

  page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(page, TRUE);

  tab->prompt_section = create_prompt_section(tab, win);
  tab->fu_row = create_follow_up_row(tab, win);
  tab->agent_row = create_agent_row(tab, win);

  tab->output_section = create_output_section(tab, win);
  tab->marked_row = create_marked_row(tab, win);
  tab->action_row = create_action_row(tab, win);

  g_object_ref_sink(tab->prompt_section);
  g_object_ref_sink(tab->fu_row);
  g_object_ref_sink(tab->agent_row);
  g_object_ref_sink(tab->output_section);
  g_object_ref_sink(tab->marked_row);
  g_object_ref_sink(tab->action_row);

  {
    tab->content_stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(tab->content_stack), FALSE);
    gtk_widget_set_vexpand(tab->content_stack, TRUE);
    gtk_box_append(GTK_BOX(page), tab->content_stack);

    tab->layout_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_stack_add_named(GTK_STACK(tab->content_stack), tab->layout_paned,
                        "paned");
    g_signal_connect(tab->layout_paned, "notify::position",
                     G_CALLBACK(on_paned_position_changed), tab);
    g_signal_connect(tab->layout_paned, "notify::position",
                     G_CALLBACK(set_paned_half_notify), tab);

    tab->pane_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    tab->pane_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_object_ref_sink(tab->pane_left);
    g_object_ref_sink(tab->pane_right);

    tab->layout_popped = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(tab->layout_popped, 12);
    gtk_widget_set_margin_end(tab->layout_popped, 12);
    gtk_widget_set_margin_top(tab->layout_popped, 12);
    gtk_widget_set_vexpand(tab->layout_popped, TRUE);
    gtk_stack_add_named(GTK_STACK(tab->content_stack), tab->layout_popped,
                        "popped");
  }

  return page;
}

GtkWidget *tab_create_label(Tab *tab, int idx, AppWindow *win);
static void on_close_tab_clicked(GtkButton *button, AppWindow *win);
static void on_tab_label_clicked(GtkGestureClick *gesture, int n_press,
                                 double x, double y, AppWindow *win);
static void on_tab_rename_activate(GtkEntry *entry, AppWindow *win);
static void on_tab_rename_cancel(GtkEntry *entry, AppWindow *win);
static void on_close_confirm_cb(GObject *source, GAsyncResult *result,
                                gpointer user_data);
static GdkContentProvider *on_tab_drag_prepare(GtkDragSource *source, double x,
                                               double y, AppWindow *win);
static GdkDragAction on_tab_drop_motion(GtkDropTarget *drop, double x, double y,
                                        AppWindow *win);
static gboolean on_tab_drop(GtkDropTarget *drop, const GValue *value, double x,
                            double y, AppWindow *win);
static void on_tab_drop_leave(GtkDropTarget *drop, AppWindow *win);

static void tab_update_status_dot(Tab *tab) {
  GtkWidget *dot;

  dot = tab->status_dot;
  if (dot == NULL)
    return;

  gtk_widget_remove_css_class(dot, "loading");
  gtk_widget_remove_css_class(dot, "finished");
  gtk_widget_remove_css_class(dot, "finished-empty");

  gtk_widget_set_tooltip_text(dot, tab->state == STATE_FAILED
                                       ? "Reply failed or was interrupted"
                                       : NULL);
  if (tab->state == STATE_LOADING)
    gtk_widget_add_css_class(dot, "loading");
  else if (tab->state == STATE_FAILED)
    gtk_widget_add_css_class(dot, "finished-empty");
  else if (tab->state == STATE_FINISHED && tab->has_unseen_output)
    gtk_widget_add_css_class(dot, "finished");
}

static void tab_drop_indicator(AppWindow *win, int target_idx) {
  guint i;
  int n_pages;
  GtkNotebook *nb;

  nb = GTK_NOTEBOOK(win->tab_bar);
  n_pages = gtk_notebook_get_n_pages(nb);
  for (i = 0; i < (guint)n_pages; i++) {
    GtkWidget *page, *label;

    page = gtk_notebook_get_nth_page(nb, (int)i);
    label = gtk_notebook_get_tab_label(nb, page);
    if (label != NULL) {
      if ((int)i == target_idx)
        gtk_widget_add_css_class(label, "tab-drop-target");
      else
        gtk_widget_remove_css_class(label, "tab-drop-target");
    }
  }
}

static void on_tab_title_changed(GtkLabel *label, GParamSpec *pspec,
                                 gpointer data) {
  (void)pspec;
  (void)data;
  /* GtkNotebook allocates scrollable tabs from their minimum widths.
   * An ellipsized label otherwise requests only enough room for an ellipsis. */
  gboolean long_title = g_utf8_strlen(gtk_label_get_text(label), -1) > 20;
  gtk_label_set_ellipsize(label, long_title ? PANGO_ELLIPSIZE_END
                                            : PANGO_ELLIPSIZE_NONE);
  gtk_label_set_width_chars(label, long_title ? 20 : -1);
}

GtkWidget *tab_create_label(Tab *tab, int idx, AppWindow *win) {
  GtkWidget *box, *label, *close_btn;
  const char *name;

  name = tab->name != NULL ? tab->name : "New Tab";
  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_hexpand(box, FALSE);

  {
    GtkWidget *dot;

    dot = gtk_image_new_from_icon_name("media-record-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(dot), 6);
    gtk_widget_add_css_class(dot, "tab-status-dot");
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(dot, 2);
    gtk_box_append(GTK_BOX(box), dot);
    tab->status_dot = dot;
  }

  label = gtk_label_new(name);
  gtk_widget_set_tooltip_text(label, name);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(label, TRUE);
  GtkWidget *title = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(title), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(title), FALSE);
  gtk_stack_add_named(GTK_STACK(title), label, "title");
  gtk_box_append(GTK_BOX(box), title);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 20);
  g_signal_connect(label, "notify::label", G_CALLBACK(on_tab_title_changed),
                   NULL);
  on_tab_title_changed(GTK_LABEL(label), NULL, NULL);
  tab->tab_name_label = label;

  {
    GtkGesture *click;

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click),
                                  "promptr-tab-title");
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_label_clicked), win);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
    g_object_set_data(G_OBJECT(box), "tab-idx", GINT_TO_POINTER(idx));
  }

  close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close_btn, "tab-close-btn");
  gtk_widget_add_css_class(close_btn, "flat");
  gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(close_btn, "Close tab");
  gtk_accessible_update_property(GTK_ACCESSIBLE(close_btn),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Close tab",
                                 -1);
  g_object_set_data(G_OBJECT(close_btn), "tab", tab);
  g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_tab_clicked), win);
  gtk_box_append(GTK_BOX(box), close_btn);

  {
    GtkDragSource *drag;

    drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_tab_drag_prepare), win);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
  }

  return box;
}

struct CloseCtx {
  AppWindow *win;
  Tab *tab;
};

static void on_close_tab_clicked(GtkButton *button, AppWindow *win) {
  GtkWidget *label;
  Tab *tab;
  guint i;

  label = GTK_WIDGET(button);
  tab = g_object_get_data(G_OBJECT(label), "tab");
  if (tab == NULL)
    return;

  for (i = 0; i < win->tabs->len; i++)
    if (g_ptr_array_index(win->tabs, i) == tab) {
      if (tab->state != STATE_LOADING && tab->has_activity &&
          runtime_config_get_bool(win->config, "tab_confirm_before_close",
                                  TAB_CONFIRM_BEFORE_CLOSE_DEFAULT)) {
        GtkAlertDialog *dlg;
        const char *buttons[] = {"Cancel", "Close", NULL};
        struct CloseCtx *ctx;

        ctx = g_new(struct CloseCtx, 1);
        ctx->win = win;
        ctx->tab = tab_ref(tab);
        dlg = gtk_alert_dialog_new("Close tab?");
        gtk_alert_dialog_set_detail(dlg, "Close this tab?");
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_set_modal(dlg, TRUE);
        gtk_alert_dialog_choose(
            dlg, app_window_dialog_parent(GTK_WINDOW(win->window)), NULL,
            on_close_confirm_cb, ctx);
        return;
      }
      close_tab(win, (int)i);
      return;
    }
}

static void on_close_confirm_cb(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  struct CloseCtx *ctx = user_data;
  GtkAlertDialog *dlg = GTK_ALERT_DIALOG(source);
  int response;

  response = gtk_alert_dialog_choose_finish(dlg, result, NULL);
  if (response == 1 && ctx->tab->win != NULL && !ctx->tab->win->destroyed) {
    guint i;

    for (i = 0; i < ctx->win->tabs->len; i++) {
      if (g_ptr_array_index(ctx->win->tabs, i) == ctx->tab) {
        close_tab(ctx->win, (int)i);
        break;
      }
    }
  }
  tab_unref(ctx->tab);
  g_object_unref(dlg);
  g_free(ctx);
}

static void finish_tab_rename(GtkEntry *entry, AppWindow *win,
                              gboolean commit) {
  if (!GTK_IS_ENTRY(entry) ||
      g_object_get_data(G_OBJECT(entry), "rename-done") != NULL)
    return;
  Tab *tab = g_object_get_data(G_OBJECT(entry), "rename-tab");
  if (win == NULL || win->destroyed || tab == NULL || tab->win != win)
    return;
  GtkWidget *stack = gtk_widget_get_parent(GTK_WIDGET(entry));
  if (!GTK_IS_STACK(stack) || !GTK_IS_LABEL(tab->tab_name_label))
    return;
  g_autoptr(GtkEntry) keep_alive = g_object_ref(entry);
  g_object_set_data(G_OBJECT(entry), "rename-done", GINT_TO_POINTER(1));
  if (commit) {
    g_autofree char *name =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    g_strstrip(name);
    if (*name) {
      g_free(tab->name);
      tab->name = g_steal_pointer(&name);
      tab->has_activity = TRUE;
    }
  }
  tab->rename_entry = NULL;
  gtk_label_set_text(GTK_LABEL(tab->tab_name_label), tab->name);
  gtk_widget_set_tooltip_text(tab->tab_name_label, tab->name);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "title");
  gtk_stack_remove(GTK_STACK(stack), GTK_WIDGET(entry));
  if (app_window_get_active_tab(win) == tab)
    update_window_title(win, tab->name);
  if (commit)
    tab_save(tab);
}

static gboolean on_tab_rename_key(GtkEventControllerKey *controller,
                                  guint keyval, guint keycode,
                                  GdkModifierType state, AppWindow *win) {
  (void)keycode;
  (void)state;
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;
  GtkWidget *entry =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  on_tab_rename_cancel(GTK_ENTRY(entry), win);
  Tab *active = app_window_get_active_tab(win);
  if (active != NULL)
    set_prompt_focused(active);
  return GDK_EVENT_STOP;
}

static void on_tab_label_clicked(GtkGestureClick *gesture, int n_press,
                                 double x, double y, AppWindow *win) {
  (void)x;
  (void)y;
  if (win == NULL || win->destroyed)
    return;
  GtkWidget *box =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "tab-idx"));
  if (idx < 0 || idx >= (int)win->tabs->len)
    return;
  Tab *tab = g_ptr_array_index(win->tabs, idx);
  if (n_press != 2) {
    tab_switch_to(win, idx);
    return;
  }
  if (tab->rename_entry != NULL || !GTK_IS_LABEL(tab->tab_name_label))
    return;
  GtkWidget *stack = gtk_widget_get_parent(tab->tab_name_label);
  if (!GTK_IS_STACK(stack))
    return;
  GtkWidget *entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), tab->name);
  gtk_editable_set_width_chars(GTK_EDITABLE(entry),
                               CLAMP(g_utf8_strlen(tab->name, -1), 8, 20));
  gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 20);
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Tab name", -1);
  g_object_set_data_full(G_OBJECT(entry), "rename-tab", tab_ref(tab),
                         (GDestroyNotify)tab_unref);
  g_signal_connect(entry, "activate", G_CALLBACK(on_tab_rename_activate), win);
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_tab_rename_key), win);
  gtk_widget_add_controller(entry, keys);
  gtk_stack_add_named(GTK_STACK(stack), entry, "rename");
  tab->rename_entry = entry;
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "rename");
  gtk_widget_grab_focus(entry);
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

static void on_tab_rename_activate(GtkEntry *entry, AppWindow *win) {
  finish_tab_rename(entry, win, TRUE);
  Tab *active = app_window_get_active_tab(win);
  if (active != NULL)
    set_prompt_focused(active);
}

static void on_tab_rename_cancel(GtkEntry *entry, AppWindow *win) {
  finish_tab_rename(entry, win, FALSE);
}

static void on_notebook_page_switched(GtkNotebook *notebook, GtkWidget *page,
                                      guint page_num, AppWindow *win) {
  Tab *old_tab;

  (void)notebook;
  (void)page;

  if (win->restoring_session || win->tabs == NULL || page_num >= win->tabs->len)
    return;

  old_tab = app_window_get_active_tab(win);
  if (old_tab != NULL && old_tab->output_popped)
    toggle_popout(old_tab);

  if (old_tab != NULL && old_tab->rename_entry != NULL)
    on_tab_rename_cancel(GTK_ENTRY(old_tab->rename_entry), win);

  old_tab = app_window_get_active_tab(win);
  if (old_tab != NULL && old_tab->page != page && old_tab->hydrated) {
    if (old_tab->output_popped)
      toggle_popout(old_tab);
    if (tab_save(old_tab))
      tab_release_view(old_tab);
  }
  win->active_tab_idx = (int)page_num;
  disarm_escape(win);

  if (page_num < win->tabs->len) {
    Tab *tab;

    tab = g_ptr_array_index(win->tabs, page_num);
    if (tab != NULL) {
      hydrate_tab(tab);
      tab->has_unseen_output = FALSE;
      tab_update_status_dot(tab);
      apply_layout(tab);
      set_prompt_focused(tab);
      update_window_title(win, tab->name);

      /* Update provider action state + menu for the switched-to tab */
      if (tab->provider_name != NULL) {
        if (win->provider_action != NULL)
          g_simple_action_set_state(win->provider_action,
                                    g_variant_new_string(tab->provider_name));
        if (win->provider_submenu != NULL)
          rebuild_provider_menu(win->provider_submenu, tab);
        rebuild_agent_menu(win, tab);
        app_window_providers_changed(win);
      }
    }
  }
  session_write(win);
}

static void tab_switch_to(AppWindow *win, int idx) {
  if (idx < 0 || idx >= (int)win->tabs->len)
    return;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), idx);
}

static Tab *add_new_tab(AppWindow *win) {
  Tab *tab;
  GtkWidget *page;
  GtkWidget *label_widget;
  g_autofree char *name = NULL;
  int idx;

  name = g_strdup_printf("New Tab");
  tab = tab_new(win, name);
  {
    g_autofree char *layout =
        runtime_config_get_string(win->config, "layout", LAYOUT_DEFAULT);
    tab->layout_mode = g_strcmp0(layout, "horizontal") == 0 ? 0 : 1;
  }

  g_ptr_array_add(win->tabs, tab);
  idx = win->tabs->len - 1;

  page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  tab->page = page;
  label_widget = tab_create_label(tab, idx, win);

  gtk_notebook_append_page(GTK_NOTEBOOK(win->tab_bar), page, label_widget);
  return tab;
}

typedef struct {
  Tab *tab;
} CloseTabContext;
static void close_tab_response(GObject *source, GAsyncResult *result,
                               gpointer data) {
  CloseTabContext *ctx = data;
  int choice =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
  Tab *tab = ctx->tab;
  if (choice == 1 && tab->win != NULL && !tab->win->destroyed) {
    tab->close_requested = TRUE;
    if (tab->state == STATE_LOADING)
      cancel_tab(tab);
    else {
      for (guint i = 0; i < tab->win->tabs->len; i++)
        if (g_ptr_array_index(tab->win->tabs, i) == tab) {
          close_tab(tab->win, i);
          break;
        }
    }
  }
  tab_unref(tab);
  g_free(ctx);
}

static void close_tab(AppWindow *win, int idx) {
  if (idx < 0 || idx >= (int)win->tabs->len)
    return;
  Tab *closing = g_ptr_array_index(win->tabs, idx);
  if (closing->state == STATE_LOADING && !closing->close_requested) {
    GtkAlertDialog *dialog =
        gtk_alert_dialog_new("Cancel the pending reply and close this tab?");
    const char *buttons[] = {"Keep open", "Cancel and close", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    CloseTabContext *ctx = g_new0(CloseTabContext, 1);
    ctx->tab = tab_ref(closing);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_choose(dialog,
                            app_window_dialog_parent(GTK_WINDOW(win->window)),
                            NULL, close_tab_response, ctx);
    g_object_unref(dialog);
    return;
  }
  if (closing->state == STATE_LOADING) {
    cancel_tab(closing);
    return;
  }
  closing->is_open = FALSE;
  if (!tab_save(closing)) {
    closing->is_open = TRUE;
    return;
  }
  if (closing->storage_failed) {
    closing->is_open = TRUE;
    tab_save(closing);
    show_error_dialog(
        GTK_WINDOW(win->window), "Conversation has unsaved output",
        "Free disk space and choose Latest messages to retry saving "
        "before closing this tab.");
    return;
  }

  if (win->tabs->len <= 1) {
    /* The replacement page stays lazy until the closing page is removed. */
    add_new_tab(win);
    idx = win->tabs->len - 2;
  }

  if (idx < 0 || idx >= (int)win->tabs->len)
    return;

  if (closing->output_popped)
    toggle_popout(closing);
  tab_release_view(closing);
  closing->page = NULL;
  closing->win = NULL;
  win->restoring_session = TRUE;
  gtk_notebook_remove_page(GTK_NOTEBOOK(win->tab_bar), idx);
  g_ptr_array_remove_index(win->tabs, idx);
  win->restoring_session = FALSE;

  if (win->tabs->len == 0)
    add_new_tab(win);

  {
    guint j;
    GtkNotebook *nb;

    nb = GTK_NOTEBOOK(win->tab_bar);
    for (j = 0; j < (guint)gtk_notebook_get_n_pages(nb); j++) {
      GtkWidget *lb;

      lb =
          gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, (int)j));
      if (lb != NULL)
        g_object_set_data(G_OBJECT(lb), "tab-idx", GINT_TO_POINTER(j));
    }
  }

  win->active_tab_idx =
      gtk_notebook_get_current_page(GTK_NOTEBOOK(win->tab_bar));
  Tab *active = app_window_get_active_tab(win);
  if (active != NULL)
    hydrate_tab(active);
  session_write(win);
}

static GdkContentProvider *on_tab_drag_prepare(GtkDragSource *source, double x,
                                               double y, AppWindow *win) {
  GtkWidget *label_box;
  int idx;
  Tab *tab;

  (void)win;
  (void)x;
  (void)y;

  label_box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
  idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(label_box), "tab-idx"));
  if (idx < 0 || (guint)idx >= win->tabs->len)
    return NULL;
  tab = g_ptr_array_index(win->tabs, idx);
  return gdk_content_provider_new_typed(G_TYPE_STRING, tab->id);
}

static GdkDragAction on_tab_drop_motion(GtkDropTarget *drop, double x, double y,
                                        AppWindow *win) {
  guint i;
  int n_pages;
  GtkNotebook *nb;
  graphene_rect_t bounds;
  int target;

  (void)drop;
  (void)y;

  nb = GTK_NOTEBOOK(win->tab_bar);
  n_pages = gtk_notebook_get_n_pages(nb);
  target = n_pages;

  for (i = 0; i < (guint)n_pages; i++) {
    GtkWidget *lp, *ll;

    lp = gtk_notebook_get_nth_page(nb, (int)i);
    ll = gtk_notebook_get_tab_label(nb, lp);
    if (ll != NULL && gtk_widget_compute_bounds(ll, GTK_WIDGET(nb), &bounds)) {
      if (x < bounds.origin.x + bounds.size.width / 2.0) {
        target = (int)i;
        break;
      }
    }
  }

  tab_drop_indicator(win, target);
  return GDK_ACTION_MOVE;
}

static gboolean on_tab_drop(GtkDropTarget *drop, const GValue *value, double x,
                            double y, AppWindow *win) {
  int src_idx;
  guint i;
  int n_pages;
  GtkNotebook *nb;
  graphene_rect_t bounds;
  int target;
  GtkWidget *apage, *alabel;

  (void)drop;
  (void)y;

  tab_drop_indicator(win, -1);
  src_idx = -1;
  {
    const char *uuid = g_value_get_string(value);

    for (i = 0; i < win->tabs->len; i++) {
      Tab *t = g_ptr_array_index(win->tabs, i);

      if (g_strcmp0(t->id, uuid) == 0) {
        src_idx = (int)i;
        break;
      }
    }
  }
  if (src_idx < 0)
    return TRUE;

  nb = GTK_NOTEBOOK(win->tab_bar);
  n_pages = gtk_notebook_get_n_pages(nb);
  target = n_pages;

  for (i = 0; i < (guint)n_pages; i++) {
    GtkWidget *lp, *ll;

    lp = gtk_notebook_get_nth_page(nb, (int)i);
    ll = gtk_notebook_get_tab_label(nb, lp);
    if (ll != NULL && gtk_widget_compute_bounds(ll, GTK_WIDGET(nb), &bounds)) {
      if (x < bounds.origin.x + bounds.size.width / 2.0) {
        target = (int)i;
        break;
      }
    }
  }

  if (target == src_idx || target == src_idx + 1)
    return TRUE;

  apage = gtk_notebook_get_nth_page(nb, src_idx);
  alabel = gtk_notebook_get_tab_label(nb, apage);
  if (apage != NULL)
    g_object_ref(apage);
  if (alabel != NULL)
    g_object_ref(alabel);

  win->restoring_session = TRUE;
  Tab *previous = app_window_get_active_tab(win);
  if (previous != NULL && previous->page != apage && previous->hydrated) {
    if (previous->output_popped)
      toggle_popout(previous);
    if (tab_save(previous))
      tab_release_view(previous);
  }
  gtk_notebook_remove_page(nb, src_idx);

  {
    Tab *tab;

    tab = g_ptr_array_index(win->tabs, src_idx);
    g_ptr_array_steal_index(win->tabs, src_idx);

    if (src_idx < target)
      target--;

    g_ptr_array_insert(win->tabs, target, tab);
  }

  gtk_notebook_insert_page(nb, apage, alabel, target);
  gtk_notebook_set_current_page(nb, target);
  win->active_tab_idx = target;
  win->restoring_session = FALSE;
  hydrate_tab(g_ptr_array_index(win->tabs, target));
  session_write(win);

  {
    int j;

    for (j = 0; j < gtk_notebook_get_n_pages(nb); j++) {
      GtkWidget *lb;

      lb = gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, j));
      if (lb != NULL)
        g_object_set_data(G_OBJECT(lb), "tab-idx", GINT_TO_POINTER(j));
    }
  }

  g_object_unref(apage);
  g_object_unref(alabel);
  return TRUE;
}

static void on_tab_drop_leave(GtkDropTarget *drop, AppWindow *win) {
  (void)drop;
  tab_drop_indicator(win, -1);
}

static void on_new_tab_clicked(AppWindow *win) {
  Tab *tab;

  tab = add_new_tab(win);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), win->tabs->len - 1);
  hydrate_tab(tab);
  session_write(win);
  apply_layout(tab);
  setup_tooltips(tab, win);
  update_cmd_preview(tab);
}

static void on_restore_tab(AppWindow *win) {
  g_autofree char *dir = NULL;
  GDir *gdir;
  const char *name;
  g_autofree char *best_uuid = NULL;
  g_autofree char *best_time = NULL;

  {
    const char *data_dir;

    data_dir = g_get_user_data_dir();
    dir = g_build_filename(data_dir, DATA_DIR_SUFFIX, "tabs", NULL);
  }

  gdir = g_dir_open(dir, 0, NULL);
  if (gdir == NULL)
    return;

  for (;;) {
    name = g_dir_read_name(gdir);
    if (name == NULL)
      break;
    if (!g_str_has_suffix(name, ".conf") && !g_str_has_suffix(name, ".meta"))
      continue;

    {
      g_autoptr(GKeyFile) kf = NULL;
      g_autofree char *uuid = NULL;
      gboolean is_open;
      gboolean has_activity;
      g_autofree char *time_str = NULL;

      kf = g_key_file_new();
      g_autofree char *candidate = g_strndup(name, strlen(name) - 5);
      g_clear_pointer(&kf, g_key_file_unref);
      kf = tab_read_metadata(candidate);
      if (kf == NULL)
        continue;

      is_open = g_key_file_get_boolean(kf, "tab", "is_open", NULL);
      has_activity = g_key_file_get_boolean(kf, "tab", "has_activity", NULL);
      if (is_open || !has_activity)
        continue;

      uuid = g_strndup(name, strlen(name) - strlen(".conf"));
      time_str = g_key_file_get_string(kf, "tab", "time", NULL);

      if (best_time == NULL ||
          (time_str != NULL && g_strcmp0(time_str, best_time) > 0)) {
        g_free(best_uuid);
        g_free(best_time);
        best_uuid = g_strdup(uuid);
        best_time = g_strdup(time_str);
      }
    }
  }
  g_dir_close(gdir);

  if (best_uuid == NULL)
    return;

  {
    Tab *tab;

    tab = tab_load(win, best_uuid);
    if (tab != NULL) {
      tab->is_open = TRUE;
      tab_save(tab);
      gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar),
                                    win->tabs->len - 1);
      apply_layout(tab);
      setup_tooltips(tab, win);
    }
  }
}

static gboolean tab_strip_empty_at(GtkWidget *notebook, double x, double y) {
  GtkWidget *picked = gtk_widget_pick(notebook, x, y, GTK_PICK_DEFAULT);
  gboolean in_header = FALSE;
  for (GtkWidget *widget = picked; widget != NULL && widget != notebook;
       widget = gtk_widget_get_parent(widget)) {
    const char *name = gtk_widget_get_css_name(widget);
    if (GTK_IS_BUTTON(widget) || g_strcmp0(name, "tab") == 0 ||
        g_strcmp0(name, "arrow") == 0)
      return FALSE;
    if (g_strcmp0(name, "header") == 0 &&
        gtk_widget_get_parent(widget) == notebook)
      in_header = TRUE;
  }
  return in_header;
}

static void on_tab_bar_double_click(GtkGestureClick *gesture, int n_press,
                                    double x, double y, AppWindow *win) {
  if (win == NULL || win->destroyed || !GTK_IS_NOTEBOOK(win->tab_bar))
    return;
  gboolean empty = tab_strip_empty_at(win->tab_bar, x, y);
  if (n_press == 1) {
    g_object_set_data(G_OBJECT(gesture), "first-click-empty",
                      GINT_TO_POINTER(empty));
    return;
  }
  gboolean first_empty = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(gesture), "first-click-empty"));
  g_object_set_data(G_OBJECT(gesture), "first-click-empty", NULL);
  if (n_press != 2 || !empty || !first_empty)
    return;
  if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture)) !=
      NULL)
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  on_new_tab_clicked(win);
}

static GtkWidget *create_status_bar(AppWindow *win) {
  GtkWidget *outer, *inner, *ver_label;

  (void)win;

  outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(outer, TRUE);
  gtk_widget_add_css_class(outer, "status-bar");

  inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start(inner, 12);
  gtk_widget_set_margin_end(inner, 12);
  gtk_widget_set_margin_top(inner, 5);
  gtk_widget_set_margin_bottom(inner, 5);
  gtk_box_append(GTK_BOX(outer), inner);

  win->status_bar = gtk_label_new("Ready");
  gtk_label_set_xalign(GTK_LABEL(win->status_bar), 0.0f);
  gtk_widget_set_hexpand(win->status_bar, TRUE);
  gtk_box_append(GTK_BOX(inner), win->status_bar);

  ver_label =
      gtk_label_new("<span size='small' alpha='40%'>v" VERSION "</span>");
  gtk_label_set_use_markup(GTK_LABEL(ver_label), TRUE);
  gtk_box_append(GTK_BOX(inner), ver_label);

  return outer;
}

static void setup_tooltips(Tab *tab, AppWindow *win) {
  if (tab == NULL || !tab->hydrated || win == NULL || win->destroyed)
    return;
  g_autofree char *t_copy = shortcut_human(win->config, "kb_copy_marked");
  g_autofree char *t_submit = shortcut_human(win->config, "kb_submit");
  g_autofree char *t_cancel = shortcut_human(win->config, "kb_cancel");
  char *tip;

  tip = g_strdup_printf("Copy marked lines: %s", t_copy);
  status_bar_on_hover(tab->copy_btn, win, tip);
  g_free(tip);

  tip = g_strdup_printf("Submit prompt: %s", t_submit);
  status_bar_on_hover(tab->submit_btn, win, tip);
  g_free(tip);
  if (strlen(t_cancel) > 0) {
    tip = g_strdup_printf("Cancel reply: esc, esc or %s", t_cancel);
  } else {
    tip = g_strdup("Cancel reply: esc, esc");
  }
  status_bar_on_hover(tab->cancel_btn, win, tip);
  g_free(tip);

  status_bar_on_hover(tab->follow_up_check, win,
                      "Include saved messages in the next request");
  g_autofree char *prompt_hint =
      *t_submit ? g_strdup_printf("Submit with %s. Press enter for a new line.",
                                  t_submit)
                : g_strdup("Use Submit to send. Press enter for a new line.");
  g_autofree char *output_hint =
      *t_copy
          ? g_strdup_printf("Mark lines in the gutter. Copy with %s.", t_copy)
          : g_strdup("Mark lines in the gutter, then use Copy Marked Lines.");
  status_bar_on_hover(tab->prompt_view, win, prompt_hint);
  status_bar_on_hover(tab->output_view, win, output_hint);
  add_dropdown_hover(tab->agent_dropdown, win);
  add_dropdown_hover(tab->model_dropdown, win);
}

static void _box_remove_all(GtkBox *box) {
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
    gtk_box_remove(box, child);
}

static void apply_layout(Tab *tab) {
  if (tab == NULL || !GTK_IS_PANED(tab->layout_paned) ||
      !GTK_IS_BOX(tab->pane_left) || !GTK_IS_BOX(tab->pane_right) ||
      !GTK_IS_BOX(tab->layout_popped))
    return;
  _box_remove_all(GTK_BOX(tab->pane_left));
  _box_remove_all(GTK_BOX(tab->pane_right));
  _box_remove_all(GTK_BOX(tab->layout_popped));

  gtk_widget_set_visible(tab->popout_btn, !tab->output_popped);
  gtk_widget_set_visible(tab->prompt_btns,
                         !tab->output_popped && tab->layout_mode == 1);
  gtk_widget_set_visible(tab->agent_btns,
                         !tab->output_popped && tab->layout_mode == 0);

  if (tab->output_popped) {
    GtkWidget *spacer;

    gtk_box_append(GTK_BOX(tab->layout_popped), tab->prompt_section);
    gtk_box_append(GTK_BOX(tab->layout_popped), tab->fu_row);
    gtk_box_append(GTK_BOX(tab->layout_popped), tab->agent_row);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(tab->layout_popped), spacer);

    gtk_box_append(GTK_BOX(tab->layout_popped), tab->action_row);

    gtk_stack_set_visible_child_name(GTK_STACK(tab->content_stack), "popped");
  } else {
    GtkOrientation orientation;

    orientation = tab->layout_mode == 0 ? GTK_ORIENTATION_VERTICAL
                                        : GTK_ORIENTATION_HORIZONTAL;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(tab->layout_paned),
                                   orientation);

    gtk_paned_set_start_child(GTK_PANED(tab->layout_paned), tab->pane_left);
    gtk_paned_set_end_child(GTK_PANED(tab->layout_paned), tab->pane_right);

    gtk_widget_set_margin_start(tab->pane_left, 12);
    gtk_widget_set_margin_end(tab->pane_left,
                              orientation == GTK_ORIENTATION_VERTICAL ? 12 : 4);
    gtk_widget_set_margin_top(tab->pane_left, 12);

    gtk_widget_set_margin_start(tab->pane_right, 12);
    gtk_widget_set_margin_end(tab->pane_right, 12);
    gtk_widget_set_margin_top(tab->pane_right, 12);

    gtk_widget_set_size_request(tab->pane_left, 200, 160);
    gtk_widget_set_size_request(tab->pane_right, 200, 200);

    gtk_box_append(GTK_BOX(tab->pane_left), tab->prompt_section);
    gtk_box_append(GTK_BOX(tab->pane_left), tab->fu_row);
    gtk_box_append(GTK_BOX(tab->pane_left), tab->agent_row);

    gtk_box_append(GTK_BOX(tab->pane_right), tab->output_section);
    gtk_box_append(GTK_BOX(tab->pane_right), tab->marked_row);
    gtk_box_append(GTK_BOX(tab->pane_right), tab->action_row);

    gtk_paned_set_shrink_start_child(GTK_PANED(tab->layout_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(tab->layout_paned), FALSE);

    set_paned_half(GTK_PANED(tab->layout_paned), tab);

    gtk_stack_set_visible_child_name(GTK_STACK(tab->content_stack), "paned");
    gtk_widget_grab_focus(tab->prompt_view);
  }
}

static gboolean on_popout_close_request(GtkWindow *window, Tab *tab) {
  (void)window;
  toggle_popout(tab);
  return TRUE;
}

static void toggle_popout(Tab *tab) {
  if (tab == NULL || tab->win == NULL || tab->win->destroyed)
    return;
  if (!tab->hydrated)
    hydrate_tab(tab);
  if (!tab->hydrated || !GTK_IS_BOX(tab->action_row) ||
      !GTK_IS_BOX(tab->output_section) || !GTK_IS_BOX(tab->marked_row) ||
      !GTK_IS_BUTTON(tab->copy_btn))
    return;
  AppWindow *win = tab->win;

  if (!tab->output_popped) {
    GtkWidget *popup, *popup_box, *btn, *crow;
    struct PopupEscCtx *ctx;
    GtkEventController *pctrl;

    tab->output_popped = TRUE;
    apply_layout(tab);

    gtk_box_remove(GTK_BOX(tab->action_row), tab->copy_btn);

    popup = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(popup), "Promptr — Output");
    gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(win->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(popup), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(popup), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(popup), 700, 400);

    ctx = g_new(struct PopupEscCtx, 1);
    ctx->data = tab;
    ctx->close_fn = (void (*)(gpointer))toggle_popout;
    pctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(popup, pctrl);
    g_signal_connect_data(pctrl, "key-pressed", G_CALLBACK(on_popup_esc), ctx,
                          (GClosureNotify)esc_ctx_free, 0);
    g_signal_connect(popup, "close-request",
                     G_CALLBACK(on_popout_close_request), tab);

    popup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(popup_box, 12);
    gtk_widget_set_margin_end(popup_box, 12);
    gtk_widget_set_margin_top(popup_box, 8);
    gtk_widget_set_margin_bottom(popup_box, 8);
    gtk_window_set_child(GTK_WINDOW(popup), popup_box);

    gtk_box_append(GTK_BOX(popup_box), tab->output_section);
    gtk_box_append(GTK_BOX(popup_box), tab->marked_row);

    crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(crow), tab->copy_btn);
    btn = gtk_button_new_with_label("Dock");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(toggle_popout), tab);
    gtk_box_append(GTK_BOX(crow), btn);
    gtk_box_append(GTK_BOX(popup_box), crow);

    tab->popout_window = popup;
    gtk_window_present(GTK_WINDOW(popup));
  } else {
    GtkWidget *popup, *popup_box, *crow;

    popup = tab->popout_window;
    if (popup != NULL && GTK_IS_WINDOW(popup)) {
      popup_box = gtk_window_get_child(GTK_WINDOW(popup));
      if (popup_box != NULL && GTK_IS_BOX(popup_box)) {
        gtk_box_remove(GTK_BOX(popup_box), tab->output_section);
        gtk_box_remove(GTK_BOX(popup_box), tab->marked_row);

        crow = gtk_widget_get_parent(tab->copy_btn);
        if (crow != NULL && GTK_IS_BOX(crow))
          gtk_box_remove(GTK_BOX(crow), tab->copy_btn);
      }
      gtk_window_destroy(GTK_WINDOW(popup));
    }
    tab->popout_window = NULL;
    tab->output_popped = FALSE;

    gtk_box_prepend(GTK_BOX(tab->action_row), tab->copy_btn);
    apply_layout(tab);
  }
}

AppWindow *app_window_new(GtkApplication *app) {
  AppWindow *win;
  GtkWidget *main_box;
  GtkWidget *status_bar_widget;
  Tab *tab;
  win = g_new0(AppWindow, 1);

  win->app = app;
  win->active_tab_idx = -1;
  win->config = runtime_config_load();

  {
    const char *data_dir;
    g_autofree char *log_dir = NULL;
    g_autofree char *log_path = NULL;
    char timestr[64];
    time_t now;
    struct tm tm;

    data_dir = g_get_user_data_dir();
    log_dir = g_build_filename(data_dir, DATA_DIR_SUFFIX, "logs", NULL);
    g_mkdir_with_parents(log_dir, 0700);

    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestr, sizeof(timestr), "promptr-%Y%m%dT%H%M%S.log", &tm);
    log_path = g_build_filename(log_dir, timestr, NULL);
    win->log_file = fopen(log_path, "w");
    if (win->log_file != NULL) {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);
      fprintf(win->log_file, "Promptr v" VERSION ": session %s\n", timestr);
      fflush(win->log_file);
    }
  }
  app_style_apply(win->config);

  app_window_reload_shortcuts(win);

  win->tabs = g_ptr_array_new_with_free_func((GDestroyNotify)tab_unref);

  win->window = gtk_window_new();
  gtk_window_set_application(GTK_WINDOW(win->window), app);
  {
    GtkSettings *settings;

    settings = gtk_widget_get_settings(win->window);
    g_object_set(settings, "gtk-cursor-aspect-ratio", 0.08, NULL);
  }
  update_window_title(win, "New Tab");
  gtk_window_set_default_size(
      GTK_WINDOW(win->window),
      runtime_config_get_int(win->config, "width", DEFAULT_WIDTH),
      runtime_config_get_int(win->config, "height", DEFAULT_HEIGHT));
  gtk_window_set_resizable(GTK_WINDOW(win->window), TRUE);

  if (runtime_config_get_bool(win->config, "layer_shell",
                              LAYER_SHELL_ENABLED)) {
    gtk_window_set_decorated(GTK_WINDOW(win->window), FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(win->window));
    gtk_layer_set_layer(GTK_WINDOW(win->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win->window), DATA_DIR_SUFFIX);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  } else {
    gtk_window_set_decorated(
        GTK_WINDOW(win->window),
        runtime_config_get_bool(win->config, "decorated", DECORATED_DEFAULT));
  }

  g_signal_connect(win->window, "close-request", G_CALLBACK(on_close_request),
                   win);

  {
    GtkEventController *winctrl;

    winctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(win->window, winctrl);
    g_signal_connect(winctrl, "key-pressed", G_CALLBACK(on_window_key_pressed),
                     win);
  }

  main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win->window), main_box);

  {
    GSimpleActionGroup *actions;
    gboolean bar_visible;
    GSimpleAction *sact;

    actions = g_simple_action_group_new();
    GSimpleAction *manage = g_simple_action_new("manage_providers", NULL);
    g_signal_connect_swapped(manage, "activate",
                             G_CALLBACK(on_manage_providers), win);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(manage));
    g_object_unref(manage);
    const char *context_actions[] = {
        "choose_project", "clear_project",  "choose_agent", "manual_model",
        "older_history",  "latest_history", "focus_prompt"};
    for (guint i = 0; i < G_N_ELEMENTS(context_actions); i++) {
      GSimpleAction *context = g_simple_action_new(context_actions[i], NULL);
      g_signal_connect(context, "activate", G_CALLBACK(on_context_action), win);
      g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(context));
      g_object_unref(context);
    }
    GSimpleAction *histories = g_simple_action_new("history_list", NULL);
    g_signal_connect_swapped(histories, "activate", G_CALLBACK(on_history_list),
                             win);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(histories));
    g_object_unref(histories);

    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("new_tab", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("close_tab", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("restore_tab", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("close", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("quit", NULL)));
    g_action_map_add_action(
        G_ACTION_MAP(actions),
        G_ACTION(g_simple_action_new_stateful("toggle_follow_up", NULL,
                                              g_variant_new_boolean(FALSE))));
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(g_simple_action_new(
                                                       "toggle_layout", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("session_log", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("shortcuts", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("about", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("submit", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("cancel", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("popout", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new("copy_marked", NULL)));

    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "close_tab"),
        "activate", G_CALLBACK(on_menu_close_tab), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "toggle_follow_up"),
        "activate", G_CALLBACK(on_menu_toggle_follow_up), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "toggle_layout"),
        "activate", G_CALLBACK(on_menu_toggle_layout), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "popout"), "activate",
        G_CALLBACK(on_menu_popout), win, NULL, 0);

    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "new_tab"),
        "activate", G_CALLBACK(on_new_tab_clicked), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "restore_tab"),
        "activate", G_CALLBACK(on_restore_tab), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "close"), "activate",
        G_CALLBACK(on_close), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "quit"), "activate",
        G_CALLBACK(on_quit), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "session_log"),
        "activate", G_CALLBACK(on_log), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "shortcuts"),
        "activate", G_CALLBACK(on_shortcuts), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "about"), "activate",
        G_CALLBACK(on_about), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "submit"), "activate",
        G_CALLBACK(on_submit), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cancel"), "activate",
        G_CALLBACK(on_cancel), win);
    g_signal_connect_swapped(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "copy_marked"),
        "activate", G_CALLBACK(on_copy), win);
    g_action_map_add_action(
        G_ACTION_MAP(actions),
        G_ACTION(g_simple_action_new("cycle_agent_next", NULL)));
    g_action_map_add_action(
        G_ACTION_MAP(actions),
        G_ACTION(g_simple_action_new("cycle_agent_prev", NULL)));
    g_action_map_add_action(
        G_ACTION_MAP(actions),
        G_ACTION(g_simple_action_new("cycle_model_next", NULL)));
    g_action_map_add_action(
        G_ACTION_MAP(actions),
        G_ACTION(g_simple_action_new("cycle_model_prev", NULL)));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new_stateful(
                                "cycle_agent_set", G_VARIANT_TYPE_STRING,
                                g_variant_new_string("None"))));
    g_action_map_add_action(G_ACTION_MAP(actions),
                            G_ACTION(g_simple_action_new_stateful(
                                "cycle_model_set", G_VARIANT_TYPE_STRING,
                                g_variant_new_string("None"))));
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_agent_next"),
        "activate", G_CALLBACK(on_menu_cycle_agent_next), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_agent_prev"),
        "activate", G_CALLBACK(on_menu_cycle_agent_prev), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_model_next"),
        "activate", G_CALLBACK(on_menu_cycle_model_next), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_model_prev"),
        "activate", G_CALLBACK(on_menu_cycle_model_prev), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_agent_set"),
        "activate", G_CALLBACK(on_menu_cycle_agent_set), win, NULL, 0);
    g_signal_connect_data(
        g_action_map_lookup_action(G_ACTION_MAP(actions), "cycle_model_set"),
        "activate", G_CALLBACK(on_menu_cycle_model_set), win, NULL, 0);

    /* Provider selection — stateful action, one radio item per known provider
     */
    {
      Tab *active_tb = app_window_get_active_tab(win);
      const char *init_p = active_tb != NULL ? active_tb->provider_name : "";
      GSimpleAction *sact;

      win->provider_action = sact = g_simple_action_new_stateful(
          "set_provider", G_VARIANT_TYPE_STRING, g_variant_new_string(init_p));
      g_signal_connect(sact, "change-state", G_CALLBACK(on_set_provider), win);
      g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(sact));
    }

    bar_visible = runtime_config_get_bool(win->config, "menu_bar_visible",
                                          MENU_BAR_VISIBLE_DEFAULT);
    sact = g_simple_action_new_stateful("menu_bar_visible", NULL,
                                        g_variant_new_boolean(bar_visible));
    g_signal_connect(sact, "change-state", G_CALLBACK(on_menu_toggle_bar), win);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(sact));
    win->menu_bar_action = sact;

    {
      gboolean sb_visible;

      sb_visible = runtime_config_get_bool(win->config, "status_bar_visible",
                                           STATUS_BAR_VISIBLE_DEFAULT);
      sact = g_simple_action_new_stateful("status_bar_visible", NULL,
                                          g_variant_new_boolean(sb_visible));
      g_signal_connect(sact, "change-state",
                       G_CALLBACK(on_menu_toggle_status_bar), win);
      g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(sact));
      win->status_bar_action = sact;
    }

    win->actions = actions;
    gtk_widget_insert_action_group(win->window, "win", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    g_autoptr(GMenuModel) menu =
        app_menu_create(win->config, &win->provider_submenu,
                        &win->agent_section_menu, &win->model_section_menu);
    rebuild_provider_menu(win->provider_submenu,
                          app_window_get_active_tab(win));
    rebuild_agent_menu(win, app_window_get_active_tab(win));
    rebuild_model_menu(win, app_window_get_active_tab(win));
    win->menu_bar = gtk_popover_menu_bar_new_from_model(menu);
    gtk_widget_set_visible(win->menu_bar, bar_visible);
    gtk_box_prepend(GTK_BOX(main_box), win->menu_bar);
  }

  log_append(win, "session \xe2\x86\x92 started");

  status_bar_widget = create_status_bar(win);
  win->status_bar_box = status_bar_widget;

  win->tab_bar = gtk_notebook_new();
  gtk_widget_add_css_class(win->tab_bar, "conversation-notebook");
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(win->tab_bar), TRUE);
  {
    g_autofree char *pos = NULL;

    pos = runtime_config_get_string(win->config, "tab_position",
                                    TAB_POSITION_DEFAULT);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(win->tab_bar),
                             g_strcmp0(pos, "bottom") == 0 ? GTK_POS_BOTTOM
                                                           : GTK_POS_TOP);
  }
  gtk_widget_set_vexpand(win->tab_bar, TRUE);

  {
    GtkDropTarget *drop;

    drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect(drop, "motion", G_CALLBACK(on_tab_drop_motion), win);
    g_signal_connect(drop, "drop", G_CALLBACK(on_tab_drop), win);
    g_signal_connect(drop, "leave", G_CALLBACK(on_tab_drop_leave), win);
    gtk_widget_add_controller(win->tab_bar, GTK_EVENT_CONTROLLER(drop));
  }

  {
    GtkGesture *click;

    click = gtk_gesture_click_new();
    gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click),
                                  "promptr-tab-strip");
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_bar_double_click),
                     win);
    gtk_widget_add_controller(win->tab_bar, GTK_EVENT_CONTROLLER(click));
  }

  gtk_box_append(GTK_BOX(main_box), win->tab_bar);
  gtk_box_append(GTK_BOX(main_box), status_bar_widget);

  {
    gboolean sb_visible;

    sb_visible = runtime_config_get_bool(win->config, "status_bar_visible",
                                         STATUS_BAR_VISIBLE_DEFAULT);
    gtk_widget_set_visible(win->status_bar_box, sb_visible);
  }

  g_signal_connect(win->tab_bar, "switch-page",
                   G_CALLBACK(on_notebook_page_switched), win);

  if (runtime_config_get_bool(win->config, "tab_show_add_button",
                              TAB_SHOW_ADD_BUTTON_DEFAULT)) {
    GtkWidget *add_btn;

    add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add_btn, "New tab");
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(add_btn), GTK_ACCESSIBLE_PROPERTY_LABEL, "New tab", -1);
    gtk_widget_add_css_class(add_btn, "flat");
    g_signal_connect_swapped(add_btn, "clicked", G_CALLBACK(on_new_tab_clicked),
                             win);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(win->tab_bar), add_btn,
                                   GTK_PACK_START);
  }

  tab = session_restore(win);

  hydrate_tab(tab);
  setup_tooltips(tab, win);
  apply_layout(tab);
  update_cmd_preview(tab);

  return win;
}
void app_window_show(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);

  gtk_window_present(GTK_WINDOW(win->window));
  if (tab != NULL)
    set_prompt_focused(tab);
}

void app_window_present(AppWindow *win) {
  GtkWindow *target = app_window_dialog_parent(GTK_WINDOW(win->window));
  gtk_window_present(target);
  if (target == GTK_WINDOW(win->window)) {
    Tab *tab = app_window_get_active_tab(win);
    if (tab != NULL)
      set_prompt_focused(tab);
  }
}

void app_window_close_and_quit(AppWindow *win) {

  if (win == NULL || win->destroyed)
    return;
  if (!win->quitting) {
    win->quitting = TRUE;
    g_application_hold(G_APPLICATION(win->app));
  }
  gboolean pending = FALSE;
  ModelOwner *models = win->model_owner;
  if (models != NULL && models->pending > 0) {
    pending = TRUE;
    if (win->model_cancel != NULL)
      g_cancellable_cancel(win->model_cancel);
  }
  for (guint j = 0; j < win->tabs->len; j++) {
    Tab *t = g_ptr_array_index(win->tabs, j);
    if (t->state == STATE_LOADING) {
      pending = TRUE;
      cancel_tab(t);
    }
  }
  if (pending) {
    set_status_text(win, "Finishing pending requests before quitting…");
    return;
  }
  for (guint j = 0; j < win->tabs->len; j++) {
    Tab *t = g_ptr_array_index(win->tabs, j);
    if (thread_store_has_unsaved(t->store) || !tab_save(t)) {
      show_error_dialog(GTK_WINDOW(win->window),
                        "Conversation could not be saved",
                        "Free disk space and choose Latest messages to retry "
                        "saving before quitting.");
      win->quitting = FALSE;
      g_application_release(G_APPLICATION(win->app));
      return;
    }
  }

  session_write(win);
  close_popups(win);
  if (win->log_file != NULL) {
    fclose(win->log_file);
    win->log_file = NULL;
  }
  /* Cancel pending timers — their callbacks would dereference freed AppWindow.
   */
  if (win->esc_reset_timeout > 0)
    g_source_remove(win->esc_reset_timeout);
  if (win->status_timeout > 0) {
    g_source_remove(win->status_timeout);
    win->status_timeout = 0;
  }
  win->destroyed = TRUE;
  for (guint j = 0; j < win->tabs->len; j++) {
    Tab *t = g_ptr_array_index(win->tabs, j);
    if (t->output_popped)
      toggle_popout(t);
    if (t->hydrated)
      tab_release_view(t);
  }
  gtk_window_destroy(GTK_WINDOW(win->window));
  g_application_release(G_APPLICATION(win->app));
  g_application_quit(G_APPLICATION(win->app));
}

void app_window_free(gpointer data) {
  AppWindow *win = data;

  if (win == NULL)
    return;
  if (win->model_owner != NULL) {
    ModelOwner *owner = win->model_owner;
    owner->win = NULL;
    model_owner_unref(owner);
  }

  for (guint i = 0; i < win->tabs->len; i++) {
    Tab *tab = g_ptr_array_index(win->tabs, i);
    if (tab->hydrated)
      tab_release_view(tab);
    tab->page = NULL;
    tab->win = NULL;
  }
  if (win->model_cancel != NULL)
    g_cancellable_cancel(win->model_cancel);
  g_clear_object(&win->model_cancel);
  g_clear_pointer(&win->model_catalogs, g_hash_table_unref);
  g_free(win->model_profile);
  g_ptr_array_free(win->tabs, TRUE);

  runtime_config_free(win->config);
  if (win->log_file != NULL)
    fclose(win->log_file);
  g_free(win);
}

/* ── state management ──────────────────────────────────────────── */

static void set_load_state_common(Tab *tab) {
  gtk_widget_set_sensitive(tab->prompt_view, FALSE);
  gtk_widget_set_sensitive(tab->agent_dropdown, FALSE);
  gtk_widget_set_sensitive(tab->model_dropdown, FALSE);
  gtk_widget_set_sensitive(tab->follow_up_check, FALSE);
  gtk_widget_set_sensitive(tab->submit_btn, FALSE);
  gtk_widget_set_visible(tab->cancel_btn, TRUE);
  gtk_widget_set_sensitive(tab->cancel_btn, TRUE);
  gtk_spinner_set_spinning(GTK_SPINNER(tab->spinner), TRUE);
  gtk_widget_set_visible(tab->spinner, TRUE);
}

static void set_load_state_idle(Tab *tab) {
  gtk_widget_set_sensitive(tab->prompt_view, TRUE);
  update_picker_sensitivity(tab);
  gtk_widget_set_sensitive(tab->follow_up_check, TRUE);
  gtk_widget_set_sensitive(tab->submit_btn, TRUE);
  gtk_widget_set_visible(tab->cancel_btn, FALSE);
  gtk_widget_set_sensitive(tab->cancel_btn, FALSE);
  gtk_spinner_set_spinning(GTK_SPINNER(tab->spinner), FALSE);
  gtk_widget_set_visible(tab->spinner, FALSE);
}

static void set_loading_state(Tab *tab, const char *cmd) {
  AppWindow *win = tab->win;

  tab->state = STATE_LOADING;
  set_load_state_common(tab);
  tab_update_status_dot(tab);

  log_append(win, "submit → %s%s", tab->follow_up_active ? "[follow-up] " : "",
             cmd);

  gtk_button_set_label(GTK_BUTTON(tab->submit_btn), "Running...");

  set_status_text(win, "Reply in progress. Press esc twice to cancel.");

  update_submit_sensitivity(tab);
}

static void on_submit(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);
  if (tab == NULL || !tab->hydrated || tab->state == STATE_LOADING ||
      tab->loading_history)
    return;
  if (thread_store_has_unsaved(tab->store)) {
    show_error_dialog(
        GTK_WINDOW(win->window), "Conversation has unsaved output",
        "Choose Latest to retry saving before starting another request.");
    return;
  }
  g_autofree char *query = get_trimmed_text(tab->prompt_view);
  if (query == NULL || !*query)
    return;
  if (tab->project_dir != NULL && *tab->project_dir &&
      (!g_path_is_absolute(tab->project_dir) ||
       !g_file_test(tab->project_dir, G_FILE_TEST_IS_DIR))) {
    show_error_dialog(
        GTK_WINDOW(win->window), "Project unavailable",
        "Choose an existing project directory before submitting.");
    return;
  }
  g_autoptr(GError) error = NULL;
  g_autofree char *agent = get_selected_text(tab->agent_dropdown);
  if (tab->provider != NULL)
    provider_manager_destroy(tab->provider);
  tab->provider =
      provider_manager_create_named(win->config, tab->provider_name, &error);
  if (tab->provider == NULL) {
    show_provider_error(tab, "Provider unavailable",
                        error != NULL
                            ? error->message
                            : "Configure this provider in Preferences.",
                        tab->provider_name);
    return;
  }
  tab_capture_input(tab);
  if (tab->store == NULL)
    tab->store = thread_store_new(tab->id);
  g_free(tab->request_id);
  tab->request_id = g_uuid_string_random();
  g_free(tab->request_profile);
  tab->request_profile = g_strdup(tab->provider_name);
  tab->follow_up_active = tab->follow_up;
  ProviderRequest request = {.thread_id = tab->id,
                             .request_id = tab->request_id,
                             .query = query,
                             .model = tab->selected_model,
                             .agent_name = agent,
                             .agent_content = NULL,
                             .project_dir = tab->project_dir,
                             .store = tab->store,
                             .follow_up = tab->follow_up};
  g_clear_object(&tab->cancellable);
  tab->cancellable = g_cancellable_new();
  tab_ref(tab);
  if (!tab->provider->vtable->submit(tab->provider->impl, tab, &request,
                                     tab->cancellable, on_provider_event, NULL,
                                     &error)) {
    tab_unref(tab);
    show_error_dialog(GTK_WINDOW(win->window), "Submission failed",
                      error != NULL ? error->message
                                    : "Provider could not start");
    return;
  }
  tab->has_activity = TRUE;
  tab_auto_rename(tab);
  tab->close_requested = FALSE;
  tab->rendered_bytes = 0;
  g_free(tab->cmd_string);
  tab->cmd_string = g_strdup("Provider request");
  GtkTextBuffer *buf =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  gtk_text_buffer_set_text(buf, "", -1);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter(buf, &start);
  gtk_text_buffer_create_mark(buf, "stream-pos", &start, FALSE);
  set_loading_state(tab, tab->cmd_string);
  tab_save(tab);
}

static void on_provider_event(gpointer source, const ProviderEvent *event,
                              gpointer user_data) {
  (void)user_data;
  Tab *tab = source;
  if (tab == NULL)
    return;
  gboolean terminal = event->type != PROVIDER_EVENT_CHUNK;
  AppWindow *win = tab->win;
  if (win == NULL || win->destroyed) {
    if (terminal)
      tab_unref(tab);
    return;
  }
  if (event->type == PROVIDER_EVENT_CHUNK) {
    tab->has_unseen_output = app_window_get_active_tab(win) != tab;
    tab_update_status_dot(tab);
    if (tab->loading_history)
      tab->refresh_again = TRUE;
    if (tab->hydrated && !tab->loading_history && event->output != NULL) {
      if (event->output_end <= tab->rendered_bytes)
        return;
      gsize length = strlen(event->output);
      guint64 start_offset = event->output_end - length;
      gsize skip =
          tab->rendered_bytes > start_offset
              ? MIN((guint64)length, tab->rendered_bytes - start_offset)
              : 0;
      GtkTextBuffer *buf =
          gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
      GtkTextIter end;
      GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "stream-pos");
      if (mark != NULL)
        gtk_text_buffer_get_iter_at_mark(buf, &end, mark);
      else
        gtk_text_buffer_get_end_iter(buf, &end);
      gtk_text_buffer_insert(buf, &end, event->output + skip, -1);
      tab->rendered_bytes = event->output_end;
      if (gtk_text_buffer_get_char_count(buf) > 64 * 1024) {
        GtkTextIter start, trim;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_iter_at_offset(
            buf, &trim, gtk_text_buffer_get_char_count(buf) - 64 * 1024);
        gtk_text_buffer_delete(buf, &start, &trim);
      }
    }
    return;
  }
  tab->state = event->type == PROVIDER_EVENT_DONE              ? STATE_FINISHED
               : event->error_msg != NULL && *event->error_msg ? STATE_FAILED
                                                               : STATE_CANCELED;
  tab->storage_failed = thread_store_has_unsaved(tab->store);
  tab->has_unseen_output = app_window_get_active_tab(win) != tab;
  if (tab->provider != NULL) {
    provider_manager_destroy(tab->provider);
    tab->provider = NULL;
  }
  g_clear_object(&tab->cancellable);
  g_clear_pointer(&tab->cmd_string, g_free);
  tab_update_status_dot(tab);
  if (tab->hydrated) {
    set_load_state_idle(tab);
    gtk_button_set_label(GTK_BUTTON(tab->submit_btn), "Submit");
    if (tab->loading_history)
      tab->refresh_again = TRUE;
    else
      refresh_history(tab, FALSE);
    if (app_window_get_active_tab(win) == tab)
      set_status_text(win, "Ready");
  } else if (!tab->storage_failed) {
    thread_store_unref(tab->store);
    tab->store = NULL;
  }
  if (event->type == PROVIDER_EVENT_ERROR && event->error_msg != NULL &&
      *event->error_msg) {
    tab->has_unseen_output = TRUE;
    g_autofree char *title = g_strdup_printf("Reply failed: %s", tab->name);
    show_provider_error(tab, title, event->error_msg, tab->request_profile);
  }
  tab_save(tab);
  if (tab->close_requested && !win->quitting) {
    for (guint i = 0; i < win->tabs->len; i++)
      if (g_ptr_array_index(win->tabs, i) == tab) {
        close_tab(win, i);
        break;
      }
  }
  if (win->quitting)
    app_window_close_and_quit(win);
  tab_unref(tab);
}

/* ── cancel ────────────────────────────────────────────────────── */

static void on_cancel(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);

  if (tab == NULL)
    return;
  if (tab->state != STATE_LOADING)
    return;
  log_append(win, "cancel → user requested");
  if (tab->provider != NULL)
    tab->provider->vtable->cancel(tab->provider->impl);
}

/* ── per-tab / all-tabs cancel helpers ─────────────────────────── */

static void cancel_tab(Tab *tab) {
  if (tab == NULL || tab->state != STATE_LOADING)
    return;
  if (tab->provider != NULL)
    tab->provider->vtable->cancel_tab(tab->provider->impl, tab);
}

static void on_follow_up_toggled(Tab *tab) {
  tab->follow_up =
      gtk_check_button_get_active(GTK_CHECK_BUTTON(tab->follow_up_check));
  update_cmd_preview(tab);
  update_submit_sensitivity(tab);
}

static gboolean esc_arm_timeout_cb(gpointer data) {
  AppWindow *win = data;
  Tab *tab;

  win->esc_armed = FALSE;
  win->esc_reset_timeout = 0;

  tab = app_window_get_active_tab(win);
  if (tab != NULL && tab->state == STATE_LOADING)
    set_status_text(win, "Reply in progress. Press esc twice to cancel.");
  else
    set_status_text(win, "Ready");
  return G_SOURCE_REMOVE;
}

static void disarm_escape(AppWindow *win) {
  if (win->esc_armed) {
    win->esc_armed = FALSE;
    if (win->esc_reset_timeout != 0) {
      g_source_remove(win->esc_reset_timeout);
      win->esc_reset_timeout = 0;
    }
  }
}

/* ── copy ──────────────────────────────────────────────────────── */

static gboolean status_pop_cb(gpointer data) {
  AppWindow *win = data;

  win->status_timeout = 0;
  if (!win->destroyed)
    set_status_text(win, "Ready");
  return G_SOURCE_REMOVE;
}

static void set_status_text(AppWindow *win, const char *text) {
  Tab *tab;

  tab = app_window_get_active_tab(win);
  if (tab != NULL && tab->provider_name != NULL && text != NULL) {
    g_autoptr(ProviderProfile) profile =
        provider_profile_load(tab->provider_name, NULL);
    g_autofree char *prefixed = g_strdup_printf(
        "[%s] %s", profile != NULL ? profile->name : "Choose provider", text);

    gtk_label_set_text(GTK_LABEL(win->status_bar), prefixed);
  } else {
    gtk_label_set_text(GTK_LABEL(win->status_bar), text != NULL ? text : "");
  }
}

static void on_copy(AppWindow *win) {
  Tab *tab;
  g_autofree char *text = NULL;
  GdkClipboard *clipboard;
  size_t nlines;
  g_autofree char *msg = NULL;

  if (win == NULL || win->destroyed)
    return;
  tab = app_window_get_active_tab(win);
  if (tab == NULL || !tab->hydrated || !GTK_IS_TEXT_VIEW(tab->output_view))
    return;

  text = get_marked_text(tab);
  if (text == NULL)
    return;
  clipboard = gdk_display_get_clipboard(gtk_widget_get_display(win->window));
  gdk_clipboard_set_text(clipboard, text);

  nlines = 0;
  if (text != NULL && text[0] != '\0') {
    nlines = 1;
    for (const char *p = text; *p != '\0'; p++)
      if (*p == '\n')
        nlines++;
  }

  if (runtime_config_get_bool(win->config, "notify_on_copy", NOTIFY_ON_COPY)) {
    GNotification *n;
    n = g_notification_new("Promptr: copied to clipboard");
    if (text != NULL && text[0] != '\0') {
      g_autofree char *preview = NULL;
      gsize len;
      len = strlen(text);
      if (len > 50) {
        preview = g_strndup(text, 50);
        g_notification_set_body(n, preview);
      } else {
        g_notification_set_body(n, text);
      }
    }
    g_application_send_notification(G_APPLICATION(win->app), "copy", n);
    g_object_unref(n);
  }

  msg = g_strdup_printf("%zu %s copied to clipboard", nlines,
                        nlines == 1 ? "line" : "lines");
  set_status_text(win, msg);
  if (win->status_timeout > 0)
    g_source_remove(win->status_timeout);
  win->status_timeout = g_timeout_add_seconds(2, status_pop_cb, win);

  log_append(win, "copy → %zu chars to clipboard",
             text != NULL ? strlen(text) : 0);
}

/* ── close / quit ──────────────────────────────────────────────── */

static void on_close(AppWindow *win) {
  Tab *tab = app_window_get_active_tab(win);
  if (tab != NULL)
    tab_save(tab);
  session_write(win);
  close_popups(win);
  gtk_widget_set_visible(win->window, FALSE);
}

static void on_quit(AppWindow *win) { app_window_close_and_quit(win); }

/* ── reusable error dialog ────────────────────────────────────── */

GtkWindow *app_window_dialog_parent(GtkWindow *parent) {
  if (!GTK_IS_WINDOW(parent))
    return parent;
  GtkWindow *best = parent;
  guint best_depth = 0;
  GListModel *windows = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    g_autoptr(GtkWindow) candidate = g_list_model_get_item(windows, i);
    if (!gtk_widget_get_visible(GTK_WIDGET(candidate)) ||
        !gtk_window_get_modal(candidate))
      continue;
    GtkWindow *ancestor = candidate;
    guint depth = 0;
    while (ancestor != NULL && ancestor != parent && depth < 100) {
      ancestor = gtk_window_get_transient_for(ancestor);
      depth++;
    }
    if (ancestor == parent && depth > best_depth) {
      best = candidate;
      best_depth = depth;
    }
  }
  return best;
}

void show_error_dialog(GtkWindow *parent, const char *title,
                       const char *detail) {
  GtkWidget *alert_win, *alert_box, *alert_title, *alert_detail, *alert_dismiss;
  GtkEventController *k;

  alert_win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(alert_win), title);
  gtk_window_set_transient_for(GTK_WINDOW(alert_win),
                               app_window_dialog_parent(parent));
  gtk_window_set_modal(GTK_WINDOW(alert_win), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(alert_win), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(alert_win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(alert_win), 420, -1);
  gtk_widget_add_css_class(alert_win, "alert-dialog");

  alert_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(alert_box, 20);
  gtk_widget_set_margin_end(alert_box, 20);
  gtk_widget_set_margin_top(alert_box, 20);
  gtk_widget_set_margin_bottom(alert_box, 20);
  gtk_window_set_child(GTK_WINDOW(alert_win), alert_box);

  alert_title = gtk_label_new(title);
  gtk_widget_add_css_class(alert_title, "heading");
  gtk_widget_set_halign(alert_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(alert_box), alert_title);

  alert_detail = gtk_label_new(detail);
  gtk_label_set_wrap(GTK_LABEL(alert_detail), TRUE);
  gtk_widget_set_halign(alert_detail, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(alert_box), alert_detail);

  alert_dismiss = gtk_button_new_with_label("OK");
  gtk_widget_set_halign(alert_dismiss, GTK_ALIGN_END);
  gtk_widget_set_margin_top(alert_dismiss, 8);
  g_signal_connect_swapped(alert_dismiss, "clicked",
                           G_CALLBACK(gtk_window_destroy), alert_win);
  gtk_box_append(GTK_BOX(alert_box), alert_dismiss);

  k = gtk_event_controller_key_new();
  g_signal_connect_swapped(k, "key-pressed", G_CALLBACK(gtk_window_destroy),
                           alert_win);
  gtk_widget_add_controller(alert_win, k);

  gtk_window_present(GTK_WINDOW(alert_win));
}

/* ── close / quit ─────────────────────────────────────────────── */

static void close_popups(AppWindow *win) {
  guint i;

  for (i = 0; i < win->tabs->len; i++) {
    Tab *t = g_ptr_array_index(win->tabs, i);

    if (t->output_popped)
      toggle_popout(t);
  }

  if (win->shortcuts_popup != NULL) {
    gtk_window_destroy(GTK_WINDOW(win->shortcuts_popup));
    win->shortcuts_popup = NULL;
  }
  if (win->log_popup != NULL) {
    gtk_window_destroy(GTK_WINDOW(win->log_popup));
    win->log_popup = NULL;
    win->cmd_label = NULL; /* child of the destroyed popup */
  }
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
  (void)window;
  on_close(user_data);
  return TRUE;
}

/* ── log popup ─────────────────────────────────────────────────── */

static void on_log_close(AppWindow *win) {
  if (runtime_config_get_bool(win->config, "layer_shell", LAYER_SHELL_ENABLED))
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  gtk_widget_set_visible(win->log_popup, FALSE);
}

static void on_log(AppWindow *win) {
  if (win->log_popup == NULL) {
    GtkWidget *popup, *box, *scroll, *close_btn;
    GtkTextBuffer *buf;
    GtkTextIter end;
    char timestr[64];
    time_t now;
    struct tm tm;

    popup = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(win->window));
    gtk_window_set_modal(GTK_WINDOW(popup), FALSE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(popup), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(popup), TRUE);
    gtk_window_set_title(GTK_WINDOW(popup), "Log");
    gtk_window_set_default_size(GTK_WINDOW(popup), 600, 300);
    gtk_window_set_resizable(GTK_WINDOW(popup), TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(popup), box);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    if (win->cmd_label == NULL) {
      win->cmd_label = gtk_text_view_new();
      gtk_text_view_set_editable(GTK_TEXT_VIEW(win->cmd_label), FALSE);
      gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->cmd_label), FALSE);
      gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->cmd_label),
                                  GTK_WRAP_WORD_CHAR);
      gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->cmd_label), 10);
      gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->cmd_label), 4);
      gtk_widget_set_hexpand(win->cmd_label, TRUE);
      gtk_widget_add_css_class(win->cmd_label, "monospace");
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), win->cmd_label);
    gtk_box_append(GTK_BOX(box), scroll);

    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->cmd_label));
    gtk_text_buffer_get_end_iter(buf, &end);
    {
      g_autofree char *header =
          g_strdup_printf("Promptr v" VERSION ": session %s\n", timestr);
      gtk_text_buffer_insert(buf, &end, header, -1);
    }

    close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(on_log_close),
                             win);
    gtk_box_append(GTK_BOX(box), close_btn);

    {
      struct PopupEscCtx *ctx;

      ctx = g_new(struct PopupEscCtx, 1);
      ctx->data = win;
      ctx->close_fn = (void (*)(gpointer))on_log_close;

      GtkEventController *k = gtk_event_controller_key_new();
      g_signal_connect_data(k, "key-pressed", G_CALLBACK(on_popup_esc), ctx,
                            esc_ctx_free, 0);
      gtk_widget_add_controller(popup, k);
    }

    win->log_popup = popup;
  }

  if (runtime_config_get_bool(win->config, "layer_shell", LAYER_SHELL_ENABLED))
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  if (gtk_widget_is_visible(win->log_popup))
    gtk_widget_set_visible(win->log_popup, FALSE);
  gtk_window_present(GTK_WINDOW(win->log_popup));
}

/* ── shortcuts popup ────────────────────────────────────────────── */

static void on_shortcuts_close(AppWindow *win) {
  if (runtime_config_get_bool(win->config, "layer_shell", LAYER_SHELL_ENABLED))
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  gtk_widget_set_visible(win->shortcuts_popup, FALSE);
}

static void on_shortcuts(AppWindow *win) {
  if (win->shortcuts_popup == NULL) {
    GtkWidget *popup, *box, *scroll, *grid, *close_btn;

    popup = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(win->window));
    gtk_window_set_modal(GTK_WINDOW(popup), FALSE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(popup), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(popup), TRUE);
    gtk_window_set_title(GTK_WINDOW(popup), "Keyboard Shortcuts");
    gtk_window_set_default_size(GTK_WINDOW(popup), 460, 500);
    gtk_window_set_resizable(GTK_WINDOW(popup), TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(popup), box);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_add_css_class(grid, "shortcuts-grid");

    {
      GtkWidget *c0, *c1;

      c0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand(c0, TRUE);
      gtk_widget_add_css_class(c0, "shortcuts-header");
      gtk_widget_add_css_class(c0, "shortcuts-cell");
      {
        GtkWidget *l = gtk_label_new("Shortcut");
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_widget_set_margin_start(l, 12);
        gtk_widget_set_margin_end(l, 12);
        gtk_widget_set_margin_top(l, 14);
        gtk_widget_set_margin_bottom(l, 14);
        gtk_box_append(GTK_BOX(c0), l);
      }
      gtk_grid_attach(GTK_GRID(grid), c0, 0, 0, 1, 1);

      c1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand(c1, TRUE);
      gtk_widget_add_css_class(c1, "shortcuts-header");
      gtk_widget_add_css_class(c1, "shortcuts-cell");
      gtk_widget_add_css_class(c1, "shortcuts-cell-last");
      {
        GtkWidget *l = gtk_label_new("Description");
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_widget_set_margin_start(l, 12);
        gtk_widget_set_margin_end(l, 12);
        gtk_widget_set_margin_top(l, 14);
        gtk_widget_set_margin_bottom(l, 14);
        gtk_box_append(GTK_BOX(c1), l);
      }
      gtk_grid_attach(GTK_GRID(grid), c1, 1, 0, 1, 1);
    }

    static const char *const keys[] = {
        "kb_focus_prompt",
        "kb_copy_marked",
        "kb_close",
        "kb_quit",
        "kb_log",
        "kb_shortcuts",
        "kb_submit",
        NULL,
        "kb_new_tab",
        "kb_close_tab",
        "kb_restore_tab",
        "kb_follow_up_toggle",
        "kb_menu_bar",
        "kb_status_bar",
        "kb_cycle_agent_next",
        "kb_cycle_model_next",
        "kb_cycle_agent_prev",
        "kb_cycle_model_prev",
        NULL,
    };
    gsize definition_count;
    const ShortcutDefinition *definitions =
        shortcut_definitions(&definition_count);
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
      const char *description = i == 7 ? "Cancel request" : "Switch to tab 1-9";
      g_autofree char *human = keys[i] != NULL
                                   ? shortcut_human(win->config, keys[i])
                                   : g_strdup(i == 7 ? "esc, esc" : "alt+1..9");
      for (gsize j = 0; keys[i] != NULL && j < definition_count; j++)
        if (g_str_equal(keys[i], definitions[j].key)) {
          description = definitions[j].label;
          break;
        }
      const char *row_class = i % 2 == 0 ? NULL : "shortcuts-row-even";
      gtk_grid_attach(GTK_GRID(grid),
                      cell_box(kbd_label(human), row_class, FALSE), 0, i + 1, 1,
                      1);
      gtk_grid_attach(GTK_GRID(grid),
                      cell_box(desc_label(description), row_class, TRUE), 1,
                      i + 1, 1, 1);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_box_append(GTK_BOX(box), scroll);

    close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(on_shortcuts_close), win);
    gtk_box_append(GTK_BOX(box), close_btn);

    {
      struct PopupEscCtx *ctx;

      ctx = g_new(struct PopupEscCtx, 1);
      ctx->data = win;
      ctx->close_fn = (void (*)(gpointer))on_shortcuts_close;

      GtkEventController *k = gtk_event_controller_key_new();
      g_signal_connect_data(k, "key-pressed", G_CALLBACK(on_popup_esc), ctx,
                            esc_ctx_free, 0);
      gtk_widget_add_controller(popup, k);
    }

    win->shortcuts_popup = popup;
  }

  if (runtime_config_get_bool(win->config, "layer_shell", LAYER_SHELL_ENABLED))
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  if (gtk_widget_is_visible(win->shortcuts_popup))
    gtk_widget_set_visible(win->shortcuts_popup, FALSE);
  gtk_window_present(GTK_WINDOW(win->shortcuts_popup));
}

/* ── menu bar actions ────────────────────────────────────────────── */

static void on_menu_toggle_bar(GSimpleAction *action, GVariant *state,
                               gpointer user_data) {
  AppWindow *win = user_data;
  gboolean visible;

  (void)action;
  visible = g_variant_get_boolean(state);
  g_simple_action_set_state(action, state);
  gtk_widget_set_visible(win->menu_bar, visible);
}

static void on_menu_close_tab(GSimpleAction *action, GVariant *param,
                              gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    close_tab(win, win->active_tab_idx);
}

static void on_menu_toggle_follow_up(GSimpleAction *action, GVariant *param,
                                     gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL && gtk_widget_is_sensitive(tab->follow_up_check))
    gtk_check_button_set_active(GTK_CHECK_BUTTON(tab->follow_up_check),
                                !tab->follow_up);
}

static void on_menu_toggle_layout(GSimpleAction *action, GVariant *param,
                                  gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL && !tab->output_popped) {
    tab->layout_mode = !tab->layout_mode;
    apply_layout(tab);
  }
}

static void on_menu_popout(GSimpleAction *action, GVariant *param,
                           gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    toggle_popout(tab);
}

static void on_menu_toggle_status_bar(GSimpleAction *action, GVariant *state,
                                      gpointer user_data) {
  AppWindow *win = user_data;
  gboolean visible;

  (void)action;
  visible = g_variant_get_boolean(state);
  g_simple_action_set_state(action, state);
  gtk_widget_set_visible(win->status_bar_box, visible);
}

static void on_menu_cycle_agent_next(GSimpleAction *action, GVariant *param,
                                     gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    cycle_dropdown_next(tab->agent_dropdown);
}

static void on_menu_cycle_agent_prev(GSimpleAction *action, GVariant *param,
                                     gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    cycle_dropdown_prev(tab->agent_dropdown);
}

static void on_menu_cycle_model_next(GSimpleAction *action, GVariant *param,
                                     gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    cycle_dropdown_next(tab->model_dropdown);
}

static void on_menu_cycle_model_prev(GSimpleAction *action, GVariant *param,
                                     gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  (void)param;
  tab = app_window_get_active_tab(win);
  if (tab != NULL)
    cycle_dropdown_prev(tab->model_dropdown);
}

static void on_menu_cycle_agent_set(GSimpleAction *action, GVariant *param,
                                    gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  tab = app_window_get_active_tab(win);
  if (tab != NULL && tab->agent_dropdown != NULL) {
    set_selected_text(tab->agent_dropdown, g_variant_get_string(param, NULL));
    rebuild_agent_menu(win, tab);
  }
}

static void on_menu_cycle_model_set(GSimpleAction *action, GVariant *param,
                                    gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab;

  (void)action;
  tab = app_window_get_active_tab(win);
  if (tab != NULL && tab->model_dropdown != NULL) {
    set_selected_text(tab->model_dropdown, g_variant_get_string(param, NULL));
    rebuild_model_menu(win, tab);
  }
}

static void rebuild_provider_menu(GMenu *submenu, Tab *tab) {
  (void)tab;
  g_menu_remove_all(submenu);
  g_autoptr(GPtrArray) profiles = provider_profile_list();
  for (guint i = 0; i < profiles->len; i++) {
    ProviderProfile *p = g_ptr_array_index(profiles, i);
    if (!p->tested && !p->bundled)
      continue;
    GMenuItem *item = g_menu_item_new(p->name, NULL);
    g_menu_item_set_action_and_target_value(item, "win.set_provider",
                                            g_variant_new_string(p->id));
    g_menu_append_item(submenu, item);
    g_object_unref(item);
  }
}

static void populate_selection_menu(GMenu *menu, const char *action,
                                    char **values) {
  g_menu_remove_all(menu);
  for (guint i = 0; values != NULL && values[i] != NULL; i++) {
    g_autoptr(GString) label = g_string_new(NULL);
    for (const char *p = values[i]; *p; p++) {
      g_string_append_c(label, *p);
      if (*p == '_')
        g_string_append_c(label, '_');
    }
    g_autoptr(GMenuItem) item = g_menu_item_new(label->str, NULL);
    g_menu_item_set_action_and_target_value(item, action,
                                            g_variant_new_string(values[i]));
    g_menu_append_item(menu, item);
  }
}

static void rebuild_agent_menu(AppWindow *win, Tab *tab) {
  if (win->agent_section_menu == NULL)
    return;
  g_auto(GStrv) values = tab != NULL && tab->hydrated
                             ? dropdown_values(tab->agent_dropdown)
                             : agent_context_list();
  populate_selection_menu(win->agent_section_menu, "win.cycle_agent_set",
                          values);
  g_clear_pointer(&values, g_strfreev);
  if (win->actions != NULL) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win->actions),
                                                 "cycle_agent_set");
    g_simple_action_set_state(
        G_SIMPLE_ACTION(action),
        g_variant_new_string(tab != NULL && tab->selected_agent != NULL
                                 ? tab->selected_agent
                                 : "None"));
  }
}

static void rebuild_model_menu(AppWindow *win, Tab *tab) {
  if (win->model_section_menu == NULL)
    return;
  g_auto(GStrv) values = tab != NULL && tab->hydrated
                             ? dropdown_values(tab->model_dropdown)
                             : g_strsplit("None", ",", -1);
  populate_selection_menu(win->model_section_menu, "win.cycle_model_set",
                          values);
  g_clear_pointer(&values, g_strfreev);
  if (win->actions != NULL) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win->actions),
                                                 "cycle_model_set");
    g_simple_action_set_state(
        G_SIMPLE_ACTION(action),
        g_variant_new_string(tab != NULL && tab->selected_model != NULL
                                 ? tab->selected_model
                                 : "None"));
  }
}

static void on_set_provider(GSimpleAction *action, GVariant *state,
                            gpointer user_data) {
  AppWindow *win = user_data;
  Tab *tab = app_window_get_active_tab(win);
  if (tab == NULL)
    return;
  const char *id = g_variant_get_string(state, NULL);
  g_autoptr(ProviderProfile) profile = provider_profile_load(id, NULL);
  if (profile == NULL || (!profile->tested && !profile->bundled))
    return;
  g_free(tab->provider_name);
  tab->provider_name = g_strdup(id);
  g_free(tab->selected_model);
  tab->selected_model = g_strdup(
      profile->model != NULL && *profile->model ? profile->model : "None");
  g_simple_action_set_state(action, state);
  app_window_providers_changed(win);
  tab_save(tab);
}

static void on_about(AppWindow *win) {
  const char *authors[] = {"toxdes", NULL};

  gtk_show_about_dialog(
      GTK_WINDOW(win->window), "program-name", "Promptr", "version", VERSION,
      "comments",
      "Promptr is a native GTK4 chat frontend with independent provider "
      "plugins. "
      "It provides a tabbed interface for running multiple queries "
      "in parallel, with configurable keyboard shortcuts, follow-up "
      "chains, and output persistence.",
      "copyright", "2024-2026 toxdes", "license-type", GTK_LICENSE_MIT_X11,
      "website", "https://github.com/toxdes/promptr", "website-label", "GitHub",
      "authors", authors, "wrap-license", TRUE, NULL);
}

/* ── key handling ──────────────────────────────────────────────── */

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win) {
  GdkModifierType mods;
  Tab *tab;

  (void)controller;
  (void)keycode;

  tab = app_window_get_active_tab(win);

  mods = state &
         (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);

  if (keyval == GDK_KEY_Escape) {
    if (tab != NULL && tab->state == STATE_LOADING) {
      if (win->esc_armed) {
        disarm_escape(win);
        on_cancel(win);
        return GDK_EVENT_STOP;
      }
      win->esc_armed = TRUE;
      win->esc_reset_timeout = g_timeout_add(2000, esc_arm_timeout_cb, win);
      set_status_text(win, "Press esc again to cancel.");
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if (win->kb_cancel_keyval != 0 &&
      gdk_keyval_to_lower(keyval) == win->kb_cancel_keyval &&
      mods == win->kb_cancel_mods) {
    if (tab != NULL && tab->state == STATE_LOADING) {
      disarm_escape(win);
      on_cancel(win);
      return GDK_EVENT_STOP;
    }
  }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    if (win->kb_submit_mods != 0 && mods == win->kb_submit_mods) {
      if (tab != NULL && gtk_widget_is_sensitive(tab->submit_btn) &&
          tab->state != STATE_LOADING)
        on_submit(win);
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  return GDK_EVENT_PROPAGATE;
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win) {
  GtkTextBuffer *buf;
  GtkTextIter start, end;
  GdkModifierType mods;
  Tab *tab;

  (void)controller;
  (void)keycode;

  tab = app_window_get_active_tab(win);

  keyval = gdk_keyval_to_lower(keyval);
  mods = state &
         (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);

  if (keyval == GDK_KEY_Escape) {
    if (tab != NULL && tab->state == STATE_LOADING) {
      if (win->esc_armed) {
        disarm_escape(win);
        on_cancel(win);
        return GDK_EVENT_STOP;
      }
      win->esc_armed = TRUE;
      win->esc_reset_timeout = g_timeout_add(2000, esc_arm_timeout_cb, win);
      set_status_text(win, "Press esc again to cancel.");
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if (win->kb_cancel_keyval != 0 && keyval == win->kb_cancel_keyval &&
      mods == win->kb_cancel_mods) {
    if (tab != NULL && tab->state == STATE_LOADING) {
      disarm_escape(win);
      on_cancel(win);
      return GDK_EVENT_STOP;
    }
  }

  if (keyval == win->kb_log_keyval && mods == win->kb_log_mods) {
    on_log(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_shortcuts_keyval && mods == win->kb_shortcuts_mods) {
    on_shortcuts(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_quit_keyval && mods == win->kb_quit_mods) {
    on_quit(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_close_keyval && mods == win->kb_close_mods) {
    on_close(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_copy_keyval && mods == win->kb_copy_mods) {
    if (tab != NULL && gtk_widget_is_sensitive(tab->copy_btn))
      on_copy(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_focus_keyval && mods == win->kb_focus_mods) {
    if (tab != NULL) {
      gtk_widget_grab_focus(tab->prompt_view);
      buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
      gtk_text_buffer_get_bounds(buf, &start, &end);
      gtk_text_buffer_select_range(buf, &start, &end);
    }
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_layout_keyval && mods == win->kb_layout_mods) {
    if (tab != NULL && !tab->output_popped) {
      tab->layout_mode = !tab->layout_mode;
      apply_layout(tab);
    }
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_popout_keyval && mods == win->kb_popout_mods) {
    if (tab != NULL)
      toggle_popout(tab);
    return GDK_EVENT_STOP;
  }

  if (win->kb_new_tab_keyval != 0 && keyval == win->kb_new_tab_keyval &&
      mods == win->kb_new_tab_mods) {
    on_new_tab_clicked(win);
    return GDK_EVENT_STOP;
  }

  if (win->kb_close_tab_keyval != 0 && keyval == win->kb_close_tab_keyval &&
      mods == win->kb_close_tab_mods) {
    if (tab != NULL)
      close_tab(win, win->active_tab_idx);
    return GDK_EVENT_STOP;
  }

  if (win->kb_restore_tab_keyval != 0 && keyval == win->kb_restore_tab_keyval &&
      mods == win->kb_restore_tab_mods) {
    on_restore_tab(win);
    return GDK_EVENT_STOP;
  }

  if (win->kb_follow_up_toggle_keyval != 0 &&
      keyval == win->kb_follow_up_toggle_keyval &&
      mods == win->kb_follow_up_toggle_mods) {
    if (tab != NULL && gtk_widget_is_sensitive(tab->follow_up_check))
      gtk_check_button_set_active(GTK_CHECK_BUTTON(tab->follow_up_check),
                                  !tab->follow_up);
    return GDK_EVENT_STOP;
  }

  if (win->kb_menu_bar_keyval != 0 && keyval == win->kb_menu_bar_keyval &&
      mods == win->kb_menu_bar_mods) {
    gboolean visible;

    visible = gtk_widget_is_visible(win->menu_bar);
    gtk_widget_set_visible(win->menu_bar, !visible);
    if (win->menu_bar_action != NULL)
      g_simple_action_set_state(win->menu_bar_action,
                                g_variant_new_boolean(!visible));
    return GDK_EVENT_STOP;
  }

  if (win->kb_status_bar_keyval != 0 && keyval == win->kb_status_bar_keyval &&
      mods == win->kb_status_bar_mods) {
    gboolean visible;

    visible = gtk_widget_is_visible(win->status_bar_box);
    gtk_widget_set_visible(win->status_bar_box, !visible);
    if (win->status_bar_action != NULL)
      g_simple_action_set_state(win->status_bar_action,
                                g_variant_new_boolean(!visible));
    return GDK_EVENT_STOP;
  }

  if (tab != NULL && win->kb_cycle_agent_next_keyval != 0 &&
      keyval == win->kb_cycle_agent_next_keyval &&
      mods == win->kb_cycle_agent_next_mods) {
    cycle_dropdown_next(tab->agent_dropdown);
    return GDK_EVENT_STOP;
  }
  if (tab != NULL && win->kb_cycle_model_next_keyval != 0 &&
      keyval == win->kb_cycle_model_next_keyval &&
      mods == win->kb_cycle_model_next_mods) {
    cycle_dropdown_next(tab->model_dropdown);
    return GDK_EVENT_STOP;
  }
  if (tab != NULL && win->kb_cycle_agent_prev_keyval != 0 &&
      keyval == win->kb_cycle_agent_prev_keyval &&
      mods == win->kb_cycle_agent_prev_mods) {
    cycle_dropdown_prev(tab->agent_dropdown);
    return GDK_EVENT_STOP;
  }
  if (tab != NULL && win->kb_cycle_model_prev_keyval != 0 &&
      keyval == win->kb_cycle_model_prev_keyval &&
      mods == win->kb_cycle_model_prev_mods) {
    cycle_dropdown_prev(tab->model_dropdown);
    return GDK_EVENT_STOP;
  }

  /* alt+1..9: switch to tab */
  if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9 && mods == GDK_ALT_MASK) {
    int idx;

    idx = (int)(keyval - GDK_KEY_1);
    if (idx < (int)win->tabs->len)
      tab_switch_to(win, idx);
    return GDK_EVENT_STOP;
  }

  return GDK_EVENT_PROPAGATE;
}

/* ── prompt change ─────────────────────────────────────────────── */

static void on_prompt_changed(GtkTextBuffer *buffer, Tab *tab) {
  (void)buffer;

  if (tab == NULL)
    return;

  gtk_widget_set_visible(tab->placeholder_label,
                         gtk_text_buffer_get_char_count(buffer) == 0);
  update_submit_sensitivity(tab);
  update_cmd_preview(tab);
}

static gboolean picker_has_choices(GtkWidget *widget) {
  if (!GTK_IS_DROP_DOWN(widget))
    return FALSE;
  GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(widget));
  guint count = model != NULL ? g_list_model_get_n_items(model) : 0;
  if (count != 1)
    return count > 1;
  g_autoptr(GObject) item = g_list_model_get_item(model, 0);
  return GTK_IS_STRING_OBJECT(item) &&
         g_strcmp0(gtk_string_object_get_string(GTK_STRING_OBJECT(item)),
                   "None") != 0;
}

static void update_picker_sensitivity(Tab *tab) {
  if (tab == NULL || !tab->hydrated)
    return;
  gboolean idle = tab->state != STATE_LOADING;
  if (GTK_IS_DROP_DOWN(tab->agent_dropdown))
    gtk_widget_set_sensitive(tab->agent_dropdown,
                             idle && picker_has_choices(tab->agent_dropdown));
  if (GTK_IS_DROP_DOWN(tab->model_dropdown))
    gtk_widget_set_sensitive(tab->model_dropdown,
                             idle && picker_has_choices(tab->model_dropdown));
}

static void update_submit_sensitivity(Tab *tab) {
  if (!tab->hydrated)
    return;
  g_autofree char *text = get_trimmed_text(tab->prompt_view);
  gboolean enable = tab->state != STATE_LOADING && !tab->loading_history &&
                    text != NULL && *text;
  gtk_widget_set_sensitive(tab->submit_btn, enable);
  AppWindow *win = tab->win;
  if (win == NULL || win->destroyed || win->actions == NULL ||
      app_window_get_active_tab(win) != tab)
    return;
  const char *names[] = {"submit", "cancel", "copy_marked", "toggle_follow_up"};
  gboolean enabled[] = {enable, tab->state == STATE_LOADING,
                        gtk_widget_get_sensitive(tab->copy_btn),
                        gtk_widget_get_sensitive(tab->follow_up_check)};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    GAction *action =
        g_action_map_lookup_action(G_ACTION_MAP(win->actions), names[i]);
    if (G_IS_SIMPLE_ACTION(action))
      g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled[i]);
  }
  GAction *follow = g_action_map_lookup_action(G_ACTION_MAP(win->actions),
                                               "toggle_follow_up");
  if (G_IS_SIMPLE_ACTION(follow))
    g_simple_action_set_state(G_SIMPLE_ACTION(follow),
                              g_variant_new_boolean(tab->follow_up));
}

/* ── helpers ───────────────────────────────────────────────────── */

static char *get_trimmed_text(GtkWidget *text_view) {
  GtkTextBuffer *buf;
  GtkTextIter start, end;
  char *raw, *start_ptr, *end_ptr;
  size_t len;

  if (!GTK_IS_TEXT_VIEW(text_view))
    return g_strdup("");
  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
  if (!GTK_IS_TEXT_BUFFER(buf))
    return g_strdup("");
  gtk_text_buffer_get_bounds(buf, &start, &end);
  raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

  start_ptr = raw;
  while (*start_ptr != '\0' && g_ascii_isspace(*start_ptr))
    start_ptr++;

  if (*start_ptr == '\0') {
    g_free(raw);
    return g_strdup("");
  }

  len = strlen(start_ptr);
  end_ptr = start_ptr + len - 1;
  while (end_ptr > start_ptr && g_ascii_isspace(*end_ptr))
    end_ptr--;
  *(end_ptr + 1) = '\0';

  char *result = g_strdup(start_ptr);
  g_free(raw);
  return result;
}

struct HoverCtx {
  AppWindow *win;
  char *text;
};

struct DropdownHoverCtx {
  AppWindow *win;
  GtkWidget *dropdown;
};

static void hover_enter_cb(GtkEventControllerMotion *ctrl, double x, double y,
                           gpointer data) {
  struct HoverCtx *ctx = data;
  (void)ctrl;
  (void)x;
  (void)y;
  set_status_text(ctx->win, ctx->text);
}

static void hover_leave_cb(GtkEventControllerMotion *ctrl, gpointer data) {
  AppWindow *win = data;
  (void)ctrl;
  set_status_text(win, "Ready");
}

static void hover_ctx_free(gpointer data, GClosure *closure) {
  struct HoverCtx *ctx = data;
  (void)closure;
  g_free(ctx->text);
  g_free(ctx);
}

static void dropdown_hover_enter(GtkEventControllerMotion *ctrl, double x,
                                 double y, gpointer data) {
  struct DropdownHoverCtx *ctx = data;
  g_autofree char *text = get_selected_text(ctx->dropdown);

  (void)ctrl;
  (void)x;
  (void)y;
  if (text != NULL)
    set_status_text(ctx->win, text);
}

static void dropdown_hover_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void add_dropdown_hover(GtkWidget *dropdown, AppWindow *win) {
  if (!GTK_IS_DROP_DOWN(dropdown) || win == NULL || win->destroyed)
    return;
  GtkEventController *motion;
  struct DropdownHoverCtx *ctx;

  ctx = g_new(struct DropdownHoverCtx, 1);
  ctx->win = win;
  ctx->dropdown = dropdown;

  motion = gtk_event_controller_motion_new();
  g_signal_connect_data(motion, "enter", G_CALLBACK(dropdown_hover_enter), ctx,
                        (GClosureNotify)dropdown_hover_ctx_free, 0);
  g_signal_connect_data(motion, "leave", G_CALLBACK(hover_leave_cb), win, NULL,
                        0);
  gtk_widget_add_controller(dropdown, motion);
}

static void status_bar_on_hover(GtkWidget *widget, AppWindow *win,
                                const char *text) {
  if (!GTK_IS_WIDGET(widget) || win == NULL || win->destroyed)
    return;
  GtkEventController *motion;
  struct HoverCtx *ctx;

  ctx = g_new(struct HoverCtx, 1);
  ctx->win = win;
  ctx->text = g_strdup(text);

  motion = gtk_event_controller_motion_new();
  g_signal_connect_data(motion, "enter", G_CALLBACK(hover_enter_cb), ctx,
                        (GClosureNotify)hover_ctx_free, 0);
  g_signal_connect_data(motion, "leave", G_CALLBACK(hover_leave_cb), win, NULL,
                        0);

  gtk_widget_add_controller(widget, motion);
}

static void on_prompt_focus_changed(GObject *object, GParamSpec *pspec,
                                    gpointer user_data) {
  AppWindow *win;
  Tab *tab;
  GtkEntry *entry;

  (void)pspec;

  if (!gtk_widget_has_focus(GTK_WIDGET(object)))
    return;

  win = user_data;
  tab = app_window_get_active_tab(win);
  if (tab == NULL || tab->rename_entry == NULL)
    return;

  entry = GTK_ENTRY(tab->rename_entry);
  if (!GTK_IS_ENTRY(entry))
    return;

  on_tab_rename_cancel(entry, win);
}

static void cycle_dropdown_next(GtkWidget *dropdown) {
  GListModel *model;
  guint n, sel;

  if (!GTK_IS_DROP_DOWN(dropdown))
    return;

  model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  n = g_list_model_get_n_items(model);
  if (n == 0)
    return;

  sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
  if (sel == GTK_INVALID_LIST_POSITION)
    sel = 0;
  else
    sel = (sel + 1) % n;

  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), sel);
}

static void cycle_dropdown_prev(GtkWidget *dropdown) {
  GListModel *model;
  guint n, sel;

  if (!GTK_IS_DROP_DOWN(dropdown))
    return;

  model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  n = g_list_model_get_n_items(model);
  if (n == 0)
    return;

  sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
  if (sel == GTK_INVALID_LIST_POSITION)
    sel = 0;
  else
    sel = (sel + n - 1) % n;

  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), sel);
}

static void update_window_title(AppWindow *win, const char *tab_name) {
  g_autofree char *title =
      g_strdup_printf("%s - Promptr", tab_name != NULL ? tab_name : "New Tab");
  gtk_window_set_title(GTK_WINDOW(win->window), title);
}

static void set_prompt_focused(Tab *tab) {
  gtk_widget_grab_focus(tab->prompt_view);
}

static GtkWidget *kbd_label(const char *text) {
  GtkWidget *l;

  l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "kbd");
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  return l;
}

static GtkWidget *desc_label(const char *text) {
  GtkWidget *l;

  l = gtk_label_new(text);
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  return l;
}

static GtkWidget *cell_box(GtkWidget *child, const char *row_class,
                           gboolean last_col) {
  GtkWidget *box;

  gtk_widget_set_margin_start(child, 12);
  gtk_widget_set_margin_end(child, 12);
  gtk_widget_set_margin_top(child, 8);
  gtk_widget_set_margin_bottom(child, 8);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_widget_add_css_class(box, "shortcuts-cell");
  if (last_col)
    gtk_widget_add_css_class(box, "shortcuts-cell-last");
  if (row_class != NULL)
    gtk_widget_add_css_class(box, row_class);
  gtk_box_append(GTK_BOX(box), child);
  return box;
}

void log_append(AppWindow *win, const char *fmt, ...) {
  GString *line;
  char timestr[64];
  time_t now;
  struct tm tm;
  va_list args;
  g_autofree char *msg = NULL;

  now = time(NULL);
  localtime_r(&now, &tm);
  strftime(timestr, sizeof(timestr), "[%H:%M:%S] ", &tm);

  va_start(args, fmt);
  msg = g_strdup_vprintf(fmt, args);
  va_end(args);

  line = g_string_new(timestr);
  g_string_append(line, msg);
  g_string_append_c(line, '\n');

  if (win->cmd_label != NULL && GTK_IS_TEXT_VIEW(win->cmd_label)) {
    GtkTextBuffer *buf;
    GtkTextIter end;

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->cmd_label));
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, line->str, -1);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(win->cmd_label),
                                 gtk_text_buffer_get_insert(buf), 0.0, FALSE,
                                 0.0, 0.0);
  }

  if (win->log_file != NULL) {
    fputs(line->str, win->log_file);
    fflush(win->log_file);
  }

  g_string_free(line, TRUE);
}

static gboolean hex_to_rgba(const char *hex, GdkRGBA *out) {
  unsigned int r, g, b, a_val;
  double a;

  if (hex == NULL || hex[0] != '#')
    return FALSE;

  if (sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
    a = 0.60;
  } else if (sscanf(hex + 1, "%2x%2x%2x%2x", &r, &g, &b, &a_val) == 4) {
    a = a_val / 255.0;
  } else {
    return FALSE;
  }

  out->red = r / 255.0;
  out->green = g / 255.0;
  out->blue = b / 255.0;
  out->alpha = a;
  return TRUE;
}

/* ── dropdown change ──────────────────────────────────────────── */

static void on_dropdown_changed(GObject *self, GParamSpec *pspec, Tab *tab) {
  (void)self;
  (void)pspec;

  if (tab == NULL)
    return;

  if (tab->hydrated && self == G_OBJECT(tab->model_dropdown)) {
    g_free(tab->selected_model);
    tab->selected_model = get_selected_text(tab->model_dropdown);
  }
  if (tab->hydrated && self == G_OBJECT(tab->agent_dropdown)) {
    g_free(tab->selected_agent);
    tab->selected_agent = get_selected_text(tab->agent_dropdown);
  }
  if (tab->win != NULL && tab->win->actions != NULL) {
    const char *name = self == G_OBJECT(tab->model_dropdown)
                           ? "cycle_model_set"
                           : "cycle_agent_set";
    GAction *action =
        g_action_map_lookup_action(G_ACTION_MAP(tab->win->actions), name);
    const char *value = self == G_OBJECT(tab->model_dropdown)
                            ? tab->selected_model
                            : tab->selected_agent;
    if (G_IS_SIMPLE_ACTION(action) && value != NULL)
      g_simple_action_set_state(G_SIMPLE_ACTION(action),
                                g_variant_new_string(value));
  }
  update_cmd_preview(tab);
}

/* ── cmd preview ──────────────────────────────────────────────── */

static void update_cmd_preview(Tab *tab) {
  char *query, *agent, *model;

  if (tab->state == STATE_LOADING)
    return;

  if (tab->state == STATE_FINISHED || tab->state == STATE_CANCELED)
    tab->state = STATE_IDLE;

  query = get_trimmed_text(tab->prompt_view);
  agent = get_selected_text(tab->agent_dropdown);
  model = get_selected_text(tab->model_dropdown);

  if (tab->provider != NULL) {
    g_autofree char *display = tab->provider->vtable->get_display_string(
        tab->provider->impl, query != NULL ? query : "", model, agent,
        tab->follow_up);

    (void)display;
  }

  g_free(query);
  g_free(agent);
  g_free(model);
}

/* ── gutter marks ──────────────────────────────────────────────── */

static void toggle_mark_at_iter(GtkTextBuffer *buf, GtkTextIter *iter) {
  int line;
  GSList *marks;

  line = gtk_text_iter_get_line(iter);
  marks = gtk_source_buffer_get_source_marks_at_line(GTK_SOURCE_BUFFER(buf),
                                                     line, "promptr-mark");

  if (marks != NULL) {
    for (GSList *m = marks; m != NULL; m = m->next)
      gtk_text_buffer_delete_mark(buf, GTK_TEXT_MARK(m->data));
    g_slist_free(marks);
  } else {
    gtk_source_buffer_create_source_mark(GTK_SOURCE_BUFFER(buf), NULL,
                                         "promptr-mark", iter);
  }
}

static void on_gutter_click(GtkGestureClick *gesture, int n_press, double x,
                            double y, AppWindow *win) {
  Tab *tab;
  GtkTextBuffer *buf;
  GtkTextIter iter;
  int buf_x, buf_y;

  (void)gesture;
  (void)n_press;

  tab = app_window_get_active_tab(win);
  if (tab == NULL || tab->output_view == NULL ||
      !GTK_IS_TEXT_VIEW(tab->output_view))
    return;

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(tab->output_view),
                                        GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y,
                                        &buf_x, &buf_y);

  if (buf_x >= 0)
    return;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(tab->output_view), &iter,
                                     buf_x, buf_y);
  toggle_mark_at_iter(buf, &iter);
  update_marked_label(tab);
}

static void update_marked_label(Tab *tab) {
  GtkTextBuffer *buf;
  GtkTextIter iter;
  GString *label;
  int line, marked, total;

  if (tab == NULL || tab->output_view == NULL ||
      !GTK_IS_TEXT_VIEW(tab->output_view))
    return;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  total = gtk_text_buffer_get_line_count(buf);
  marked = 0;
  line = 0;

  gtk_text_buffer_get_start_iter(buf, &iter);
  while (!gtk_text_iter_is_end(&iter)) {
    GSList *marks;
    marks = gtk_source_buffer_get_source_marks_at_line(GTK_SOURCE_BUFFER(buf),
                                                       line, "promptr-mark");
    if (marks != NULL) {
      marked++;
      g_slist_free(marks);
    }
    gtk_text_iter_forward_line(&iter);
    line++;
  }

  if (total <= 0) {
    gtk_label_set_text(GTK_LABEL(tab->marked_label), "Marked lines: none");
    return;
  }

  if (marked == 0) {
    gtk_label_set_text(GTK_LABEL(tab->marked_label), "Marked lines: none");
    return;
  }

  if (marked == total) {
    gtk_label_set_text(GTK_LABEL(tab->marked_label), "Marked lines: all");
    return;
  }

  label = g_string_new("Marked lines: ");
  line = 0;
  gtk_text_buffer_get_start_iter(buf, &iter);
  while (!gtk_text_iter_is_end(&iter)) {
    GSList *marks;
    marks = gtk_source_buffer_get_source_marks_at_line(GTK_SOURCE_BUFFER(buf),
                                                       line, "promptr-mark");
    if (marks != NULL) {
      if (label->len > 14)
        g_string_append_c(label, ',');
      g_string_append_printf(label, "%d", line + 1);
      g_slist_free(marks);
    }
    gtk_text_iter_forward_line(&iter);
    line++;
  }

  gtk_label_set_text(GTK_LABEL(tab->marked_label), label->str);
  g_string_free(label, TRUE);
}

static void apply_default_marks(Tab *tab, GtkTextBuffer *buf) {
  GtkTextIter iter;
  int i, total, line;
  gboolean mark_all;
  g_autofree char **parts = NULL;

  if (g_strcmp0(tab->marked_lines_str, "-1") == 0)
    return;

  mark_all = (g_strcmp0(tab->marked_lines_str, "0") == 0);

  if (mark_all) {
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (!gtk_text_iter_is_end(&iter)) {
      gtk_source_buffer_create_source_mark(GTK_SOURCE_BUFFER(buf), NULL,
                                           "promptr-mark", &iter);
      gtk_text_iter_forward_line(&iter);
    }
    return;
  }

  parts = g_strsplit(tab->marked_lines_str, ",", -1);
  total = gtk_text_buffer_get_line_count(buf);
  for (i = 0; parts[i] != NULL; i++) {
    line = (int)g_ascii_strtoll(parts[i], NULL, 10) - 1;
    if (line >= 0 && line < total) {
      gtk_text_buffer_get_iter_at_line(buf, &iter, line);
      gtk_source_buffer_create_source_mark(GTK_SOURCE_BUFFER(buf), NULL,
                                           "promptr-mark", &iter);
    }
  }
}

static char *get_marked_text(Tab *tab) {
  GtkTextBuffer *buf;
  GString *result;
  GtkTextIter iter;
  int line;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
  result = g_string_new("");
  line = 0;

  gtk_text_buffer_get_start_iter(buf, &iter);
  while (!gtk_text_iter_is_end(&iter)) {
    GSList *marks;

    marks = gtk_source_buffer_get_source_marks_at_line(GTK_SOURCE_BUFFER(buf),
                                                       line, "promptr-mark");
    if (marks != NULL) {
      GtkTextIter start, end;
      g_autofree char *text;

      start = iter;
      end = iter;
      gtk_text_iter_forward_to_line_end(&end);
      text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
      if (result->len > 0)
        g_string_append_c(result, '\n');
      g_string_append(result, text);
      g_slist_free(marks);
    }
    gtk_text_iter_forward_line(&iter);
    line++;
  }

  return g_string_free(result, FALSE);
}

typedef struct {
  Tab *tab;
  ThreadStore *store;
  guint generation;
  gint64 cursor;
  guint64 before;
  char *draft;
  char *marks;
  gboolean restore_draft;
} HistoryJob;

static void history_job_free(HistoryJob *job) {
  thread_store_unref(job->store);
  tab_unref(job->tab);
  g_free(job->draft);
  g_free(job->marks);
  g_free(job);
}

static void history_worker(GTask *task, gpointer source, gpointer data,
                           GCancellable *cancel) {
  (void)source;
  (void)cancel;
  HistoryJob *job = data;
  g_autoptr(GError) error = NULL;
  if (job->restore_draft) {
    g_autofree char *dir = thread_store_directory();
    g_autofree char *name = g_strconcat(job->tab->id, ".prompt", NULL);
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_file_get_contents(path, &job->draft, NULL, NULL);
  }
  ThreadPage *page =
      thread_store_page(job->store, job->cursor, job->before, &error);
  if (page != NULL) {
    g_autofree char *dir = thread_store_directory();
    g_autofree char *name = g_strconcat(job->tab->id, ".marks", NULL);
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_autofree char *hash =
        g_compute_checksum_for_string(G_CHECKSUM_SHA256, page->text, -1);
    g_autoptr(GKeyFile) marks = g_key_file_new();
    if (g_key_file_load_from_file(marks, path, G_KEY_FILE_NONE, NULL))
      job->marks = g_key_file_get_string(marks, "marks", hash, NULL);
  }
  if (page == NULL)
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_pointer(task, page, (GDestroyNotify)thread_page_free);
}

static void history_ready(GObject *source, GAsyncResult *result,
                          gpointer data) {
  (void)source;
  (void)data;
  GTask *task = G_TASK(result);
  HistoryJob *job = g_task_get_task_data(task);
  Tab *tab = job->tab;
  g_autoptr(GError) error = NULL;
  g_autoptr(ThreadPage) page = g_task_propagate_pointer(task, &error);
  if (tab->win == NULL || tab->win->destroyed || !tab->hydrated ||
      tab->view_generation != job->generation)
    return;
  tab->loading_history = FALSE;
  if (job->restore_draft) {
    tab->restoring_draft = FALSE;
    g_free(tab->draft);
    tab->draft = g_strdup(job->draft != NULL ? job->draft : "");
    GtkTextBuffer *prompt =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
    gtk_text_buffer_set_text(prompt, tab->draft, -1);
    gtk_widget_set_sensitive(tab->prompt_view, tab->state != STATE_LOADING);
    if (app_window_get_active_tab(tab->win) == tab &&
        tab->state != STATE_LOADING)
      set_prompt_focused(tab);
  }
  if (page == NULL) {
    tab->storage_failed = TRUE;
    show_error_dialog(GTK_WINDOW(tab->win->window),
                      "Could not load conversation",
                      error != NULL ? error->message : "Storage unavailable");
  } else {
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->output_view));
    gtk_text_buffer_set_text(buf, page->text, -1);
    tab->rendered_bytes = page->stream_bytes;
    GtkTextMark *old_mark = gtk_text_buffer_get_mark(buf, "stream-pos");
    if (old_mark != NULL)
      gtk_text_buffer_delete_mark(buf, old_mark);
    if (page->stream_chars >= 0) {
      GtkTextIter position;
      gtk_text_buffer_get_iter_at_offset(buf, &position, page->stream_chars);
      gtk_text_buffer_create_mark(buf, "stream-pos", &position, FALSE);
    }
    tab->history_more = page->more;
    tab->history_cursor = page->next_message;
    tab->history_before = page->before;

    gtk_widget_set_sensitive(tab->copy_btn, *page->text != '\0');
    char *defaults = tab->marked_lines_str;
    if (job->marks != NULL)
      tab->marked_lines_str = job->marks;
    apply_default_marks(tab, buf);
    tab->marked_lines_str = defaults;
    update_marked_label(tab);
  }
  GAction *older_action = g_action_map_lookup_action(
      G_ACTION_MAP(tab->win->actions), "older_history");
  if (app_window_get_active_tab(tab->win) == tab &&
      G_IS_SIMPLE_ACTION(older_action))
    g_simple_action_set_enabled(G_SIMPLE_ACTION(older_action),
                                tab->history_more);
  update_submit_sensitivity(tab);
  if (tab->refresh_again) {
    tab->refresh_again = FALSE;
    refresh_history(tab, FALSE);
  }
}

static void refresh_history(Tab *tab, gboolean older) {
  if (!tab->hydrated || tab->loading_history || (older && !tab->history_more))
    return;
  if (older && !tab_save(tab))
    return;
  if (tab->store == NULL)
    tab->store = thread_store_new(tab->id);
  tab->loading_history = TRUE;
  HistoryJob *job = g_new0(HistoryJob, 1);
  job->tab = tab_ref(tab);
  job->store = thread_store_ref(tab->store);
  job->generation = tab->view_generation;
  job->cursor = older ? tab->history_cursor : -1;
  job->before = older ? tab->history_before : 0;
  job->restore_draft = tab->restoring_draft;
  g_autoptr(GTask) task = g_task_new(NULL, NULL, history_ready, NULL);
  g_task_set_task_data(task, job, (GDestroyNotify)history_job_free);
  g_task_run_in_thread(task, history_worker);
  update_submit_sensitivity(tab);
}

static void on_load_older(GtkButton *button, Tab *tab) {
  (void)button;
  refresh_history(tab, TRUE);
}

static void hydrate_tab(Tab *tab) {
  if (tab->hydrated)
    return;
  gboolean follow_up = tab->follow_up;
  GtkWidget *content = tab_create_widgets(tab, tab->win);
  gtk_box_append(GTK_BOX(tab->page), content);
  tab->hydrated = TRUE;
  tab->view_generation++;
  set_selected_text(tab->agent_dropdown, tab->selected_agent);
  set_selected_text(tab->model_dropdown, tab->selected_model);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(tab->follow_up_check),
                              follow_up);
  GtkTextBuffer *buf =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
  gtk_text_buffer_set_text(buf, tab->draft != NULL ? tab->draft : "", -1);
  apply_layout(tab);
  setup_tooltips(tab, tab->win);
  if (tab->state == STATE_LOADING)
    set_load_state_common(tab);
  else
    set_load_state_idle(tab);
  tab->restoring_draft = TRUE;
  gtk_widget_set_sensitive(tab->prompt_view, FALSE);
  refresh_history(tab, FALSE);
}

static void chosen_file(GObject *source, GAsyncResult *result, gpointer data) {
  Tab *tab = data;
  gboolean project = GPOINTER_TO_INT(g_object_get_data(source, "project"));
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file =
      project ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                     result, &error)
              : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result,
                                            &error);
  if (file != NULL && tab->win != NULL && !tab->win->destroyed) {
    g_autofree char *path = g_file_get_path(file);
    if (path != NULL) {
      if (project) {
        g_free(tab->project_dir);
        tab->project_dir = g_strdup(path);

      } else {
        g_free(tab->selected_agent);
        tab->selected_agent = g_strdup(path);
        if (tab->hydrated)
          set_selected_text(tab->agent_dropdown, path);
      }
      update_picker_sensitivity(tab);
      tab_save(tab);
    }
  }
  tab_unref(tab);
}

static void on_choose_project(GtkButton *button, Tab *tab) {
  (void)button;
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Choose project directory");
  g_object_set_data(G_OBJECT(dialog), "project", GINT_TO_POINTER(TRUE));
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(tab->win->window), NULL,
                                chosen_file, tab_ref(tab));
  g_object_unref(dialog);
}

static void on_choose_agent(GtkButton *button, Tab *tab) {
  (void)button;
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Choose Markdown agent");
  gtk_file_dialog_open(dialog, GTK_WINDOW(tab->win->window), NULL, chosen_file,
                       tab_ref(tab));
  g_object_unref(dialog);
}

typedef struct {
  Tab *tab;
  GtkWidget *entry;
  GtkWidget *window;
} ModelEditor;
static void model_editor_free(gpointer data) {
  ModelEditor *e = data;
  tab_unref(e->tab);
  g_free(e);
}
static void model_apply(GtkButton *button, gpointer data) {
  (void)button;
  ModelEditor *e = data;
  if (e->tab->win != NULL && !e->tab->win->destroyed) {
    const char *id = gtk_editable_get_text(GTK_EDITABLE(e->entry));
    if (e->tab->hydrated)
      set_selected_text(e->tab->model_dropdown, *id ? id : "None");
    g_free(e->tab->selected_model);
    e->tab->selected_model = g_strdup(*id ? id : "None");
    update_picker_sensitivity(e->tab);
    rebuild_model_menu(e->tab->win, e->tab);
    tab_save(e->tab);
  }
  gtk_window_destroy(GTK_WINDOW(e->window));
}
static void on_manual_model(GtkButton *button, Tab *tab) {
  (void)button;
  GtkWidget *window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Model ID");
  gtk_window_set_transient_for(GTK_WINDOW(window),
                               GTK_WINDOW(tab->win->window));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_window_set_child(GTK_WINDOW(window), box);
  ModelEditor *e = g_new0(ModelEditor, 1);
  e->tab = tab_ref(tab);
  e->window = window;
  e->entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(e->entry),
                        tab->selected_model != NULL ? tab->selected_model : "");
  gtk_box_append(GTK_BOX(box), e->entry);
  GtkWidget *apply = gtk_button_new_with_label("Use model");
  gtk_box_append(GTK_BOX(box), apply);
  g_signal_connect(apply, "clicked", G_CALLBACK(model_apply), e);
  g_object_set_data_full(G_OBJECT(window), "model-editor", e,
                         model_editor_free);
  gtk_window_present(GTK_WINDOW(window));
}

static void on_manage_providers(AppWindow *win) { provider_settings_show(win); }

static void on_context_action(GSimpleAction *action, GVariant *param,
                              gpointer data) {
  (void)param;
  AppWindow *win = data;
  Tab *tab = app_window_get_active_tab(win);
  if (tab == NULL || !tab->hydrated)
    return;
  const char *name = g_action_get_name(G_ACTION(action));
  if (g_str_equal(name, "choose_project"))
    on_choose_project(NULL, tab);
  else if (g_str_equal(name, "clear_project"))
    clear_project(NULL, tab);
  else if (g_str_equal(name, "choose_agent"))
    on_choose_agent(NULL, tab);
  else if (g_str_equal(name, "manual_model"))
    on_manual_model(NULL, tab);
  else if (g_str_equal(name, "older_history") && tab->state != STATE_LOADING)
    on_load_older(NULL, tab);
  else if (g_str_equal(name, "latest_history"))
    latest_history(NULL, tab);
  else if (g_str_equal(name, "focus_prompt")) {
    set_prompt_focused(tab);
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->prompt_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_select_range(buffer, &start, &end);
  }
}

static void provider_error_preferences(GtkButton *button, Tab *tab) {
  GtkRoot *dialog = gtk_widget_get_root(GTK_WIDGET(button));
  if (tab->win != NULL && !tab->win->destroyed)
    provider_settings_show_profile(
        tab->win, g_object_get_data(G_OBJECT(dialog), "profile"));
  if (GTK_IS_WINDOW(dialog))
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void show_provider_error(Tab *tab, const char *title, const char *detail,
                                const char *profile) {
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(tab->win->window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 20);
  gtk_widget_set_margin_end(box, 20);
  gtk_widget_set_margin_top(box, 20);
  gtk_widget_set_margin_bottom(box, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), box);
  GtkWidget *label = gtk_label_new(detail);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_box_append(GTK_BOX(box), label);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), buttons);
  GtkWidget *close = gtk_button_new_with_label("Close");
  GtkWidget *preferences = gtk_button_new_with_label("Preferences");
  gtk_box_append(GTK_BOX(buttons), close);
  gtk_box_append(GTK_BOX(buttons), preferences);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                           dialog);
  g_object_set_data_full(G_OBJECT(dialog), "tab", tab_ref(tab),
                         (GDestroyNotify)tab_unref);
  g_object_set_data_full(G_OBJECT(dialog), "profile", g_strdup(profile),
                         g_free);
  g_signal_connect(preferences, "clicked",
                   G_CALLBACK(provider_error_preferences), tab);
  gtk_window_present(GTK_WINDOW(dialog));
}

typedef struct {
  ModelOwner *owner;
  char *profile;
  Provider *provider;
  GCancellable *cancel;
  guint generation;
} ModelLookup;

static void apply_provider_models(Tab *tab, char **values) {
  if (!tab->hydrated)
    return;
  GtkStringList *models = gtk_string_list_new(NULL);
  gtk_string_list_append(models, "None");
  for (guint i = 0; values != NULL && values[i] != NULL; i++)
    if (*values[i] && g_strcmp0(values[i], "None") != 0)
      gtk_string_list_append(models, values[i]);
  g_signal_handlers_block_matched(tab->model_dropdown, G_SIGNAL_MATCH_DATA, 0,
                                  0, NULL, NULL, tab);
  gtk_drop_down_set_model(GTK_DROP_DOWN(tab->model_dropdown),
                          G_LIST_MODEL(models));
  g_object_unref(models);
  set_selected_text(tab->model_dropdown, tab->selected_model);
  g_signal_handlers_unblock_matched(tab->model_dropdown, G_SIGNAL_MATCH_DATA, 0,
                                    0, NULL, NULL, tab);
  update_picker_sensitivity(tab);
  rebuild_model_menu(tab->win, tab);
}

static void models_ready(gpointer source, const ProviderEvent *event,
                         gpointer data) {
  (void)data;
  ModelLookup *job = source;
  if (event->type == PROVIDER_EVENT_CHUNK)
    return;
  AppWindow *win = job->owner->win;
  job->owner->pending--;
  if (win != NULL && !win->destroyed &&
      job->generation == win->model_generation) {
    win->model_pending = FALSE;
    if (event->type == PROVIDER_EVENT_DONE && event->models != NULL) {
      g_hash_table_replace(win->model_catalogs, g_strdup(job->profile),
                           g_strdupv(event->models));
      Tab *active = app_window_get_active_tab(win);
      if (active != NULL && g_strcmp0(active->provider_name, job->profile) == 0)
        apply_provider_models(active, event->models);
    } else if (!g_cancellable_is_cancelled(job->cancel)) {
      log_append(win, "models -> %s",
                 event->error_msg != NULL
                     ? event->error_msg
                     : "Discovery failed; using advertised models");
    }
  }
  provider_manager_destroy(job->provider);
  g_object_unref(job->cancel);
  model_owner_unref(job->owner);
  g_free(job->profile);
  g_free(job);
  if (win != NULL && !win->destroyed && win->quitting)
    app_window_close_and_quit(win);
}

void app_window_refresh_models(AppWindow *win) {
  if (win == NULL || win->destroyed)
    return;
  if (win->model_cancel != NULL)
    g_cancellable_cancel(win->model_cancel);
  g_clear_object(&win->model_cancel);
  g_clear_pointer(&win->model_profile, g_free);
  if (win->model_catalogs != NULL)
    g_hash_table_remove_all(win->model_catalogs);
  win->model_generation++;
  app_window_providers_changed(win);
}

void app_window_providers_changed(AppWindow *win) {
  if (win == NULL || win->destroyed || win->quitting)
    return;
  Tab *tab = app_window_get_active_tab(win);
  if (win->provider_submenu != NULL)
    rebuild_provider_menu(win->provider_submenu, tab);
  if (tab == NULL)
    return;
  g_autoptr(ProviderProfile) profile =
      provider_profile_load(tab->provider_name, NULL);
  if (profile == NULL && !g_uuid_string_is_valid(tab->provider_name)) {
    g_autoptr(GPtrArray) profiles = provider_profile_list();
    ProviderProfile *candidate = NULL;
    const char *wanted = tab->provider_name != NULL && *tab->provider_name
                             ? tab->provider_name
                             : PROVIDER_DEFAULT;
    for (guint i = 0; i < profiles->len; i++) {
      ProviderProfile *p = g_ptr_array_index(profiles, i);
      if ((!p->tested && !p->bundled) || g_strcmp0(p->plugin, wanted) != 0)
        continue;
      if (candidate == NULL || p->bundled)
        candidate = p;
    }
    if (candidate != NULL) {
      g_free(tab->provider_name);
      tab->provider_name = g_strdup(candidate->id);
      profile = provider_profile_copy(candidate);
    }
  }
  if (win->provider_action != NULL)
    g_simple_action_set_state(win->provider_action,
                              g_variant_new_string(tab->provider_name != NULL
                                                       ? tab->provider_name
                                                       : ""));
  if (profile == NULL || !tab->hydrated)
    return;
  if (win->model_catalogs == NULL)
    win->model_catalogs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)g_strfreev);
  if (g_strcmp0(win->model_profile, profile->id) != 0) {
    if (win->model_pending && win->model_profile != NULL)
      g_hash_table_remove(win->model_catalogs, win->model_profile);
    if (win->model_cancel != NULL)
      g_cancellable_cancel(win->model_cancel);
    g_clear_object(&win->model_cancel);
    g_free(win->model_profile);
    win->model_profile = g_strdup(profile->id);
    win->model_generation++;
  }
  char **cached = g_hash_table_lookup(win->model_catalogs, profile->id);
  apply_provider_models(tab, cached != NULL ? cached : profile->models);
  if (cached != NULL || (!profile->tested && !profile->bundled))
    return;
  /* Only the viewed provider is queried, once per profile/settings revision.
   * Manifest models are immediately usable while discovery runs or fails. */
  g_hash_table_insert(win->model_catalogs, g_strdup(profile->id),
                      profile->models != NULL ? g_strdupv(profile->models)
                                              : g_new0(char *, 1));
  ModelLookup *job = g_new0(ModelLookup, 1);
  if (win->model_owner == NULL) {
    ModelOwner *owner = g_new0(ModelOwner, 1);
    owner->refs = 1;
    owner->win = win;
    win->model_owner = owner;
  }
  job->owner = win->model_owner;
  job->owner->refs++;
  job->owner->pending++;
  job->profile = g_strdup(profile->id);
  job->generation = win->model_generation;
  job->provider = plugin_runner_create_profile(profile);
  job->cancel = g_cancellable_new();
  win->model_pending = TRUE;
  win->model_cancel = g_object_ref(job->cancel);
  g_autofree char *request_id = g_uuid_string_random();
  ProviderRequest request = {.thread_id = tab->id,
                             .request_id = request_id,
                             .query = "",
                             .discover_models = TRUE};
  g_autoptr(GError) error = NULL;
  if (!job->provider->vtable->submit(job->provider->impl, job, &request,
                                     job->cancel, models_ready, NULL, &error)) {
    ProviderEvent event = {.type = PROVIDER_EVENT_ERROR,
                           .error_msg = error != NULL
                                            ? error->message
                                            : "Model discovery unavailable"};
    models_ready(job, &event, NULL);
  }
}

static void clear_project(GtkButton *button, Tab *tab) {
  (void)button;
  g_clear_pointer(&tab->project_dir, g_free);
  tab_save(tab);
}
static void latest_history(GtkButton *button, Tab *tab) {
  (void)button;
  if (!tab_save(tab))
    return;
  refresh_history(tab, FALSE);
}

static char *session_path(void) {
  return g_build_filename(g_get_user_data_dir(), DATA_DIR_SUFFIX, "session",
                          NULL);
}

static void session_write(AppWindow *win) {
  if (win->restoring_session || win->destroyed || win->tabs == NULL)
    return;
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autoptr(GPtrArray) ids = g_ptr_array_new();
  for (guint i = 0; i < win->tabs->len; i++) {
    Tab *tab = g_ptr_array_index(win->tabs, i);
    if (tab->is_open)
      g_ptr_array_add(ids, tab->id);
  }
  g_key_file_set_string_list(kf, "session", "tabs",
                             (const char *const *)ids->pdata, ids->len);
  Tab *active = app_window_get_active_tab(win);
  if (active != NULL)
    g_key_file_set_string(kf, "session", "selected", active->id);
  g_autofree char *path = session_path();
  g_autofree char *dir = g_path_get_dirname(path);
  g_autofree char *data = g_key_file_to_data(kf, NULL, NULL);
  g_autoptr(GError) error = NULL;
  if (g_mkdir_with_parents(dir, 0700) != 0 ||
      !g_file_set_contents_full(path, data, -1,
                                G_FILE_SET_CONTENTS_CONSISTENT |
                                    G_FILE_SET_CONTENTS_DURABLE,
                                0600, &error))
    g_warning("Session metadata could not be saved: %s",
              error != NULL ? error->message : "directory unavailable");
}

static Tab *session_restore(AppWindow *win) {
  win->restoring_session = TRUE;
  g_autofree char *path = session_path();
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autofree char *selected = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    g_auto(GStrv) ids =
        g_key_file_get_string_list(kf, "session", "tabs", NULL, NULL);
    selected = g_key_file_get_string(kf, "session", "selected", NULL);
    for (guint i = 0; ids != NULL && ids[i] != NULL; i++) {
      gboolean duplicate = FALSE;
      for (guint j = 0; j < win->tabs->len; j++)
        if (g_strcmp0(((Tab *)g_ptr_array_index(win->tabs, j))->id, ids[i]) ==
            0)
          duplicate = TRUE;
      if (!duplicate)
        tab_load(win, ids[i]);
    }
  } else {
    g_autofree char *dir = thread_store_directory();
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    g_autoptr(GHashTable) seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *name;
    while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
      if (!g_str_has_suffix(name, ".meta") && !g_str_has_suffix(name, ".conf"))
        continue;
      g_autofree char *id = g_strndup(name, strlen(name) - 5);
      if (g_hash_table_contains(seen, id))
        continue;
      g_hash_table_add(seen, g_strdup(id));
      g_autoptr(GKeyFile) meta = tab_read_metadata(id);
      if (meta != NULL && g_key_file_get_boolean(meta, "tab", "is_open", NULL))
        tab_load(win, id);
    }
  }
  if (win->tabs->len == 0)
    add_new_tab(win);
  guint active = 0;
  for (guint i = 0; i < win->tabs->len; i++)
    if (g_strcmp0(((Tab *)g_ptr_array_index(win->tabs, i))->id, selected) == 0)
      active = i;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(win->tab_bar), active);
  win->active_tab_idx = active;
  win->restoring_session = FALSE;
  Tab *tab = g_ptr_array_index(win->tabs, active);
  hydrate_tab(tab);
  app_window_providers_changed(win);
  return tab;
}

typedef struct {
  char *id, *name, *time;
} SavedHistory;
static void saved_history_free(SavedHistory *h) {
  g_free(h->id);
  g_free(h->name);
  g_free(h->time);
  g_free(h);
}
static gint saved_history_compare(gconstpointer a, gconstpointer b) {
  const SavedHistory *one = *(SavedHistory *const *)a,
                     *two = *(SavedHistory *const *)b;
  return g_strcmp0(two->time, one->time);
}

typedef struct {
  AppWindow *win;
  GtkWidget *window, *list, *status;
  gboolean closed;
} HistoryWindow;
typedef struct {
  HistoryWindow *owner;
  char *id;
  GtkWidget *row;
} HistoryRow;
static void history_row_free(gpointer data) {
  HistoryRow *row = data;
  g_free(row->id);
  g_free(row);
}
static gboolean history_closed(GtkWindow *window, HistoryWindow *h) {
  (void)window;
  h->closed = TRUE;
  return FALSE;
}
static void history_visibility(GObject *object, GParamSpec *pspec,
                               HistoryWindow *h) {
  (void)pspec;
  if (!gtk_widget_get_visible(GTK_WIDGET(object)))
    h->closed = TRUE;
}

static void saved_history_worker(GTask *task, gpointer source, gpointer data,
                                 GCancellable *cancel) {
  (void)source;
  (void)data;
  (void)cancel;
  GPtrArray *out =
      g_ptr_array_new_with_free_func((GDestroyNotify)saved_history_free);
  g_autofree char *dir = thread_store_directory();
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const char *name;
  while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
    if (!g_str_has_suffix(name, ".meta") && !g_str_has_suffix(name, ".conf"))
      continue;
    g_autofree char *id = g_strndup(name, strlen(name) - 5);
    if (g_hash_table_contains(seen, id))
      continue;
    g_hash_table_add(seen, g_strdup(id));
    g_autoptr(GKeyFile) kf = tab_read_metadata(id);
    if (kf == NULL || g_key_file_get_boolean(kf, "tab", "is_open", NULL))
      continue;
    SavedHistory *h = g_new0(SavedHistory, 1);
    h->id = g_strdup(id);
    h->name = g_key_file_get_string(kf, "tab", "name", NULL);
    h->time = g_key_file_get_string(kf, "tab", "time", NULL);
    g_ptr_array_add(out, h);
  }
  g_ptr_array_sort(out, saved_history_compare);
  g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void history_restore_clicked(GtkButton *button, HistoryRow *row) {
  (void)button;
  HistoryWindow *h = row->owner;
  if (h->closed || h->win->destroyed)
    return;
  for (guint i = 0; i < h->win->tabs->len; i++) {
    Tab *open = g_ptr_array_index(h->win->tabs, i);
    if (g_strcmp0(open->id, row->id) == 0) {
      tab_switch_to(h->win, i);
      return;
    }
  }
  Tab *tab = tab_load(h->win, row->id);
  if (tab == NULL) {
    gtk_label_set_text(GTK_LABEL(h->status),
                       "Conversation could not be restored.");
    return;
  }
  tab->is_open = TRUE;
  tab_save(tab);
  tab_switch_to(h->win, h->win->tabs->len - 1);
  session_write(h->win);
  gtk_box_remove(GTK_BOX(h->list), row->row);
}

static void history_delete_worker(GTask *task, gpointer source, gpointer data,
                                  GCancellable *cancel) {
  (void)source;
  (void)cancel;
  g_autoptr(GError) error = NULL;
  if (thread_store_delete(data, &error))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, g_steal_pointer(&error));
}
static void history_delete_ready(GObject *source, GAsyncResult *result,
                                 gpointer data) {
  GtkWidget *window = GTK_WIDGET(source);
  HistoryRow *row = data;
  HistoryWindow *h = row->owner;
  g_autoptr(GError) error = NULL;
  gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
  thread_store_release_delete(row->id);
  if (!h->closed) {
    if (ok) {
      GListModel *windows = gtk_window_get_toplevels();
      for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        g_autoptr(GtkWindow) candidate = g_list_model_get_item(windows, i);
        HistoryWindow *other =
            g_object_get_data(G_OBJECT(candidate), "history-window");
        if (other == NULL || other->closed)
          continue;
        GtkWidget *child = gtk_widget_get_first_child(other->list);
        while (child != NULL) {
          GtkWidget *next = gtk_widget_get_next_sibling(child);
          HistoryRow *entry = g_object_get_data(G_OBJECT(child), "history-row");
          if (entry != NULL && g_strcmp0(entry->id, row->id) == 0)
            gtk_box_remove(GTK_BOX(other->list), child);
          child = next;
        }
      }
    } else {
      gtk_label_set_text(GTK_LABEL(h->status), error->message);
      gtk_widget_set_sensitive(row->row, TRUE);
    }
  }
  g_object_unref(row->row);
  (void)window;
}
static void history_delete_confirmed(GObject *source, GAsyncResult *result,
                                     gpointer data) {
  HistoryRow *row = data;
  HistoryWindow *h = row->owner;
  int choice =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
  if (!h->closed && !h->win->destroyed && choice == 1) {
    for (guint i = 0; i < h->win->tabs->len; i++) {
      Tab *open = g_ptr_array_index(h->win->tabs, i);
      if (g_strcmp0(open->id, row->id) == 0) {
        gtk_label_set_text(GTK_LABEL(h->status),
                           "Close this conversation before deleting it.");
        g_object_unref(row->row);
        g_object_unref(h->window);
        return;
      }
    }
    if (!thread_store_reserve_delete(row->id)) {
      g_object_unref(row->row);
      g_object_unref(h->window);
      return;
    }
    gtk_widget_set_sensitive(row->row, FALSE);
    g_object_ref(row->row);
    GTask *task = g_task_new(h->window, NULL, history_delete_ready, row);
    g_task_set_task_data(task, g_strdup(row->id), g_free);
    g_task_run_in_thread(task, history_delete_worker);
    g_object_unref(task);
  }
  g_object_unref(row->row);
  g_object_unref(h->window);
}
static void history_delete_clicked(GtkButton *button, HistoryRow *row) {
  (void)button;
  HistoryWindow *h = row->owner;
  GtkAlertDialog *dialog =
      gtk_alert_dialog_new("Delete this saved conversation permanently?");
  const char *buttons[] = {"Keep conversation", "Delete conversation", NULL};
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  g_object_ref(h->window);
  g_object_ref(row->row);
  gtk_alert_dialog_set_modal(dialog, TRUE);
  gtk_alert_dialog_choose(dialog,
                          app_window_dialog_parent(GTK_WINDOW(h->window)), NULL,
                          history_delete_confirmed, row);
  g_object_unref(dialog);
}
static void saved_history_ready(GObject *source, GAsyncResult *result,
                                gpointer data) {
  (void)data;
  GtkWidget *window = GTK_WIDGET(source);
  HistoryWindow *h = g_object_get_data(G_OBJECT(window), "history-window");
  g_autoptr(GPtrArray) histories =
      g_task_propagate_pointer(G_TASK(result), NULL);
  if (h->closed || histories == NULL)
    return;
  gtk_label_set_text(
      GTK_LABEL(h->status),
      histories->len ? "Closed conversations stay saved until you delete them."
                     : "No closed conversations. Close a tab to find it here.");
  for (guint i = 0; i < histories->len; i++) {
    SavedHistory *item = g_ptr_array_index(histories, i);
    HistoryRow *row = g_new0(HistoryRow, 1);
    row->owner = h;
    row->id = g_strdup(item->id);
    row->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label =
        gtk_label_new(item->name != NULL ? item->name : "Conversation");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row->row), label);
    GtkWidget *restore = gtk_button_new_with_label("Restore"),
              *remove = gtk_button_new_with_label("Delete");
    gtk_box_append(GTK_BOX(row->row), restore);
    gtk_box_append(GTK_BOX(row->row), remove);
    g_signal_connect(restore, "clicked", G_CALLBACK(history_restore_clicked),
                     row);
    g_signal_connect(remove, "clicked", G_CALLBACK(history_delete_clicked),
                     row);
    g_object_set_data_full(G_OBJECT(row->row), "history-row", row,
                           history_row_free);
    gtk_box_append(GTK_BOX(h->list), row->row);
  }
}
static void on_history_list(AppWindow *win) {
  HistoryWindow *h = g_new0(HistoryWindow, 1);
  h->win = win;
  h->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(h->window), "Saved conversations");
  gtk_window_set_transient_for(GTK_WINDOW(h->window), GTK_WINDOW(win->window));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(h->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(h->window), 600, 420);
  GtkWidget *scroll = gtk_scrolled_window_new(),
            *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_window_set_child(GTK_WINDOW(h->window), scroll);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  h->status = gtk_label_new("Loading saved conversations…");
  gtk_label_set_wrap(GTK_LABEL(h->status), TRUE);
  h->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(box), h->status);
  gtk_box_append(GTK_BOX(box), h->list);
  g_object_set_data_full(G_OBJECT(h->window), "history-window", h, g_free);
  g_signal_connect(h->window, "close-request", G_CALLBACK(history_closed), h);
  g_signal_connect(h->window, "notify::visible", G_CALLBACK(history_visibility),
                   h);
  gtk_window_present(GTK_WINDOW(h->window));
  g_autoptr(GTask) task =
      g_task_new(h->window, NULL, saved_history_ready, NULL);
  g_task_run_in_thread(task, saved_history_worker);
}

static char **dropdown_values(GtkWidget *dropdown) {
  GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  guint count = g_list_model_get_n_items(model);
  char **values = g_new0(char *, count + 1);
  for (guint i = 0; i < count; i++) {
    g_autoptr(GtkStringObject) item = g_list_model_get_item(model, i);
    values[i] = g_strdup(gtk_string_object_get_string(item));
  }
  return values;
}

void app_window_apply_editor_preferences(AppWindow *win) {
  if (win == NULL || win->destroyed)
    return;
  app_style_apply(win->config);
  gboolean numbers = runtime_config_get_bool(win->config, "editor_line_numbers",
                                             EDITOR_LINE_NUMBERS_DEFAULT);
  for (guint i = 0; i < win->tabs->len; i++) {
    Tab *tab = g_ptr_array_index(win->tabs, i);
    if (!tab->hydrated)
      continue;
    if (GTK_SOURCE_IS_VIEW(tab->prompt_view))
      gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(tab->prompt_view),
                                            numbers);
    if (GTK_SOURCE_IS_VIEW(tab->output_view))
      gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(tab->output_view),
                                            numbers);
  }
}

void app_window_reload_shortcuts(AppWindow *win) {
  struct {
    const char *key;
    guint *keyval;
    GdkModifierType *mods;
  } bindings[] = {
      {"kb_focus_prompt", &win->kb_focus_keyval, &win->kb_focus_mods},
      {"kb_copy_marked", &win->kb_copy_keyval, &win->kb_copy_mods},
      {"kb_close", &win->kb_close_keyval, &win->kb_close_mods},
      {"kb_quit", &win->kb_quit_keyval, &win->kb_quit_mods},
      {"kb_shortcuts", &win->kb_shortcuts_keyval, &win->kb_shortcuts_mods},
      {"kb_log", &win->kb_log_keyval, &win->kb_log_mods},
      {"kb_submit", &win->kb_submit_keyval, &win->kb_submit_mods},
      {"kb_cancel", &win->kb_cancel_keyval, &win->kb_cancel_mods},
      {"kb_layout", &win->kb_layout_keyval, &win->kb_layout_mods},
      {"kb_popout", &win->kb_popout_keyval, &win->kb_popout_mods},
      {"kb_new_tab", &win->kb_new_tab_keyval, &win->kb_new_tab_mods},
      {"kb_close_tab", &win->kb_close_tab_keyval, &win->kb_close_tab_mods},
      {"kb_restore_tab", &win->kb_restore_tab_keyval,
       &win->kb_restore_tab_mods},
      {"kb_follow_up_toggle", &win->kb_follow_up_toggle_keyval,
       &win->kb_follow_up_toggle_mods},
      {"kb_menu_bar", &win->kb_menu_bar_keyval, &win->kb_menu_bar_mods},
      {"kb_status_bar", &win->kb_status_bar_keyval, &win->kb_status_bar_mods},
      {"kb_cycle_agent_next", &win->kb_cycle_agent_next_keyval,
       &win->kb_cycle_agent_next_mods},
      {"kb_cycle_model_next", &win->kb_cycle_model_next_keyval,
       &win->kb_cycle_model_next_mods},
      {"kb_cycle_agent_prev", &win->kb_cycle_agent_prev_keyval,
       &win->kb_cycle_agent_prev_mods},
      {"kb_cycle_model_prev", &win->kb_cycle_model_prev_keyval,
       &win->kb_cycle_model_prev_mods},
  };
  for (guint i = 0; i < G_N_ELEMENTS(bindings); i++) {
    g_autofree char *value = shortcut_value(win->config, bindings[i].key);
    gtk_accelerator_parse(value, bindings[i].keyval, bindings[i].mods);
  }

  {
    struct {
      const char *name;
      guint keyval;
      GdkModifierType mods;
    } binds[] = {
        {"focus", win->kb_focus_keyval, win->kb_focus_mods},
        {"copy", win->kb_copy_keyval, win->kb_copy_mods},
        {"close", win->kb_close_keyval, win->kb_close_mods},
        {"quit", win->kb_quit_keyval, win->kb_quit_mods},
        {"log", win->kb_log_keyval, win->kb_log_mods},
        {"shortcuts", win->kb_shortcuts_keyval, win->kb_shortcuts_mods},
        {"submit", win->kb_submit_keyval, win->kb_submit_mods},
        {"cancel", win->kb_cancel_keyval, win->kb_cancel_mods},
        {"layout", win->kb_layout_keyval, win->kb_layout_mods},
        {"popout", win->kb_popout_keyval, win->kb_popout_mods},
        {"new_tab", win->kb_new_tab_keyval, win->kb_new_tab_mods},
        {"close_tab", win->kb_close_tab_keyval, win->kb_close_tab_mods},
        {"restore_tab", win->kb_restore_tab_keyval, win->kb_restore_tab_mods},
        {"follow_up_toggle", win->kb_follow_up_toggle_keyval,
         win->kb_follow_up_toggle_mods},
        {"menu_bar", win->kb_menu_bar_keyval, win->kb_menu_bar_mods},
        {"status_bar", win->kb_status_bar_keyval, win->kb_status_bar_mods},
    };
    gboolean conflict = FALSE;
    int n = (int)G_N_ELEMENTS(binds);

    for (int i = 0; i < n && !conflict; i++)
      for (int j = i + 1; j < n; j++)
        if (binds[i].keyval != 0 && binds[i].keyval == binds[j].keyval &&
            binds[i].mods == binds[j].mods) {
          g_warning("Shortcut conflict: \"%s\" and \"%s\" both resolve to "
                    "the same key combination",
                    binds[i].name, binds[j].name);
          conflict = TRUE;
          break;
        }
  }
  if (GTK_IS_POPOVER_MENU_BAR(win->menu_bar)) {
    g_autoptr(GMenuModel) menu =
        app_menu_create(win->config, &win->provider_submenu,
                        &win->agent_section_menu, &win->model_section_menu);
    gtk_popover_menu_bar_set_menu_model(GTK_POPOVER_MENU_BAR(win->menu_bar),
                                        menu);
    Tab *tab = app_window_get_active_tab(win);
    rebuild_provider_menu(win->provider_submenu, tab);
    rebuild_agent_menu(win, tab);
    rebuild_model_menu(win, tab);
    if (GTK_IS_WINDOW(win->shortcuts_popup)) {
      gtk_window_destroy(GTK_WINDOW(win->shortcuts_popup));
      win->shortcuts_popup = NULL;
    }
  }
  if (win->tabs != NULL) {
    g_autofree char *accel = runtime_config_get_string(
        win->config, "kb_focus_prompt", KB_FOCUS_PROMPT);
    g_autofree char *human = accel_to_human(accel);
    g_autofree char *markup = g_markup_printf_escaped(
        "Prompt  <span size='x-small' foreground='#888'>%s to focus</span>",
        human);
    for (guint i = 0; i < win->tabs->len; i++) {
      Tab *tab = g_ptr_array_index(win->tabs, i);
      if (tab->hydrated && GTK_IS_TEXT_VIEW(tab->prompt_view)) {
        GtkWidget *hint =
            g_object_get_data(G_OBJECT(tab->prompt_view), "focus-hint");
        if (GTK_IS_LABEL(hint))
          gtk_label_set_markup(GTK_LABEL(hint), markup);
      }
    }
  }
}
