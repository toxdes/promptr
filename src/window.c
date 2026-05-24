#include "window.h"
#include "command.h"
#include "config.h"
#include "configfile.h"
#include "state.h"

#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum {
  STATE_IDLE,
  STATE_LOADING,
  STATE_FINISHED,
  STATE_CANCELED
} AppState;

static void on_submit(AppWindow *win);
static void on_cancel(AppWindow *win);
static void on_copy(AppWindow *win);
static void on_close(AppWindow *win);
static void on_quit(AppWindow *win);
static void on_follow_up_toggled(AppWindow *win);
static void update_submit_sensitivity(AppWindow *win);
static char *get_trimmed_text(GtkWidget *text_view);
static char *get_selected_text(GtkWidget *dropdown);
static void set_prompt_focused(AppWindow *win);

static void set_loading_state(AppWindow *win, const char *cmd);
static void set_finished_state(AppWindow *win, char *cmd, gint64 elapsed,
                               const char *output);
static void set_canceled_state(AppWindow *win, char *cmd);
static void set_errored_state(AppWindow *win, char *cmd,
                              const char *stderr_output);

static void command_finished_cb(AppWindow *win, const char *output,
                                const char *stderr_output, gint64 elapsed,
                                int exit_code, gboolean exited_cleanly);

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win);
static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win);
static gboolean esc_arm_timeout_cb(gpointer data);
static void disarm_escape(AppWindow *win);
static gboolean status_pop_cb(gpointer data);
static gboolean on_close_request(GtkWindow *window, gpointer user_data);
static void on_prompt_changed(GtkTextBuffer *buffer, AppWindow *win);
static void on_dropdown_changed(GObject *self, GParamSpec *pspec,
                                AppWindow *win);
static void update_cmd_preview(AppWindow *win);
static void on_log(AppWindow *win);
static void on_log_close(AppWindow *win);
static void on_shortcuts(AppWindow *win);
static void on_shortcuts_close(AppWindow *win);

/* Fwd: layout helpers */
static GtkWidget *create_prompt_section(AppWindow *win);
static GtkWidget *create_follow_up_row(AppWindow *win);
static GtkWidget *create_agent_row(AppWindow *win);
static GtkWidget *create_output_section(AppWindow *win);
static GtkWidget *create_marked_row(AppWindow *win);
static GtkWidget *create_action_row(AppWindow *win);
static GtkWidget *create_status_bar(AppWindow *win);
static void setup_tooltips(AppWindow *win);
static void apply_layout(AppWindow *win);
static void toggle_popout(AppWindow *win);
struct PopupEscCtx {
  AppWindow *win;
  void (*close_fn)(AppWindow *);
};

static gboolean on_popup_esc(GtkEventControllerKey *ctrl, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer data) {
  struct PopupEscCtx *ctx = data;

  (void)ctrl;
  (void)keycode;
  (void)state;
  if (keyval == GDK_KEY_Escape) {
    ctx->close_fn(ctx->win);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void esc_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}
static void app_window_restore_state(AppWindow *win);
static void set_status_text(AppWindow *win, const char *text);
static void log_append(AppWindow *win, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static gboolean hex_to_rgba(const char *hex, GdkRGBA *out);
static void on_gutter_click(GtkGestureClick *gesture, int n_press, double x,
                            double y, AppWindow *win);
static void update_marked_label(AppWindow *win);
static void apply_default_marks(AppWindow *win, GtkTextBuffer *buf);
static char *get_marked_text(AppWindow *win);
static char *accel_to_human(const char *accel);
static void status_bar_on_hover(GtkWidget *widget, AppWindow *win,
                                const char *text);
static GtkWidget *kbd_label(const char *text);
static GtkWidget *desc_label(const char *text);
static GtkWidget *cell_box(GtkWidget *child, const char *row_class,
                           gboolean last_col);

static void load_css(int prompt_font_size, int output_font_size);

/* ── layout helpers ─────────────────────────────────────────────── */

static GtkWidget *create_prompt_section(AppWindow *win) {
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
  prompt_text = g_strdup_printf(
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

    win->log_btn_top = gtk_button_new_with_label("Log...");
    g_signal_connect_swapped(win->log_btn_top, "clicked", G_CALLBACK(on_log),
                             win);
    gtk_box_append(GTK_BOX(btns), win->log_btn_top);

    win->shortcuts_btn_top = gtk_button_new_with_label("Shortcuts...");
    g_signal_connect_swapped(win->shortcuts_btn_top, "clicked",
                             G_CALLBACK(on_shortcuts), win);
    gtk_box_append(GTK_BOX(btns), win->shortcuts_btn_top);

    gtk_box_append(GTK_BOX(hdr), btns);
    win->prompt_btns = btns;
  }

  gtk_box_append(GTK_BOX(box), hdr);

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),
                                                   TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);

  win->prompt_view = GTK_WIDGET(gtk_source_view_new());
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(win->prompt_view),
                                        TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->prompt_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->prompt_view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->prompt_view), 4);
  gtk_widget_add_css_class(win->prompt_view, "monospace");
  gtk_widget_add_css_class(win->prompt_view, "prompt-font");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), win->prompt_view);

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
    gtk_widget_add_css_class(plabel, "monospace");
    gtk_widget_add_css_class(plabel, "prompt-font");
    gtk_widget_set_can_target(plabel, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), plabel);
    win->placeholder_label = plabel;
  }

  gtk_box_append(GTK_BOX(box), overlay);

  {
    GtkTextBuffer *buffer;
    GtkEventController *key_ctrl;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->prompt_view));
    g_signal_connect(buffer, "changed", G_CALLBACK(on_prompt_changed), win);

    key_ctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(win->prompt_view, key_ctrl);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_prompt_key_pressed),
                     win);
  }

  return box;
}

static GtkWidget *create_follow_up_row(AppWindow *win) {
  GtkWidget *furow;

  furow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_top(furow, 4);
  gtk_widget_set_margin_bottom(furow, 4);

  win->follow_up_check =
      gtk_check_button_new_with_label("This prompt is a follow up");
  gtk_widget_set_sensitive(win->follow_up_check, FALSE);
  g_signal_connect_swapped(win->follow_up_check, "toggled",
                           G_CALLBACK(on_follow_up_toggled), win);
  gtk_box_append(GTK_BOX(furow), win->follow_up_check);

  return furow;
}

static GtkWidget *create_agent_row(AppWindow *win) {
  GtkWidget *row, *label, *filler;
  GtkStringList *list;
  g_autofree char **opts = NULL;
  int i;
  gboolean has_options;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  label = gtk_label_new("Agent");
  gtk_widget_set_margin_end(label, 4);
  gtk_box_append(GTK_BOX(row), label);

  opts = runtime_config_get_string_list(win->config, "agent_options");
  if (opts == NULL)
    opts = g_strsplit(DEFAULT_AGENT_OPTIONS, ",", -1);
  has_options = FALSE;
  list = gtk_string_list_new(NULL);
  for (i = 0; opts[i] != NULL && opts[i][0] != '\0'; i++) {
    gtk_string_list_append(list, opts[i]);
    has_options = TRUE;
  }
  has_options = (i > 1);
  win->agent_dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(win->agent_dropdown), 0);
  gtk_widget_set_sensitive(win->agent_dropdown, has_options);
  g_signal_connect(win->agent_dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), win);
  gtk_box_append(GTK_BOX(row), win->agent_dropdown);
  g_strfreev(opts);
  opts = NULL;

  label = gtk_label_new("Model");
  gtk_widget_set_margin_end(label, 4);
  gtk_box_append(GTK_BOX(row), label);

  opts = runtime_config_get_string_list(win->config, "model_options");
  if (opts == NULL)
    opts = g_strsplit(DEFAULT_MODEL_OPTIONS, ",", -1);
  has_options = FALSE;
  list = gtk_string_list_new(NULL);
  for (i = 0; opts[i] != NULL && opts[i][0] != '\0'; i++) {
    gtk_string_list_append(list, opts[i]);
    has_options = TRUE;
  }
  has_options = (i > 1);
  win->model_dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(win->model_dropdown), 0);
  gtk_widget_set_sensitive(win->model_dropdown, has_options);
  g_signal_connect(win->model_dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_changed), win);
  gtk_box_append(GTK_BOX(row), win->model_dropdown);
  g_strfreev(opts);

  win->submit_btn = gtk_button_new_with_label("Submit");
  gtk_widget_set_margin_start(win->submit_btn, 8);
  gtk_widget_add_css_class(win->submit_btn, "suggested-action");
  gtk_widget_set_sensitive(win->submit_btn, FALSE);
  g_signal_connect_swapped(win->submit_btn, "clicked", G_CALLBACK(on_submit),
                           win);
  gtk_box_append(GTK_BOX(row), win->submit_btn);

  win->spinner = gtk_spinner_new();
  gtk_widget_set_visible(win->spinner, FALSE);
  gtk_widget_set_margin_start(win->spinner, 8);
  gtk_widget_set_valign(win->spinner, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), win->spinner);

  win->cancel_btn = gtk_button_new_with_label("Cancel");
  gtk_widget_set_valign(win->cancel_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(win->cancel_btn, 8);
  gtk_widget_set_visible(win->cancel_btn, FALSE);
  g_signal_connect_swapped(win->cancel_btn, "clicked", G_CALLBACK(on_cancel),
                           win);
  gtk_box_append(GTK_BOX(row), win->cancel_btn);

  filler = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(filler, TRUE);
  gtk_box_append(GTK_BOX(row), filler);

  {
    GtkWidget *btns;

    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    win->log_btn = gtk_button_new_with_label("Log...");
    gtk_widget_set_valign(win->log_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(win->log_btn, "clicked", G_CALLBACK(on_log), win);
    gtk_box_append(GTK_BOX(btns), win->log_btn);

    win->shortcuts_btn = gtk_button_new_with_label("Shortcuts...");
    gtk_widget_set_valign(win->shortcuts_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(win->shortcuts_btn, "clicked",
                             G_CALLBACK(on_shortcuts), win);
    gtk_box_append(GTK_BOX(btns), win->shortcuts_btn);

    gtk_box_append(GTK_BOX(row), btns);
    win->agent_btns = btns;
  }

  return row;
}

static GtkWidget *create_output_section(AppWindow *win) {
  GtkWidget *box, *scroll, *popout_btn;
  GdkRGBA c;
  g_autofree char *color;

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  {
    GtkWidget *hdr;

    hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(hdr, TRUE);

    win->output_label = gtk_label_new("Output");
    gtk_label_set_xalign(GTK_LABEL(win->output_label), 0.0f);
    gtk_widget_set_margin_top(win->output_label, 8);
    gtk_widget_set_margin_bottom(win->output_label, 4);
    gtk_widget_set_hexpand(win->output_label, TRUE);
    gtk_box_append(GTK_BOX(hdr), win->output_label);

    popout_btn = gtk_button_new_with_label("Undock...");
    gtk_widget_set_tooltip_text(popout_btn, "Pop out output (ctrl+o)");
    gtk_widget_set_halign(popout_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(popout_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(popout_btn, "clicked", G_CALLBACK(toggle_popout),
                             win);
    gtk_box_append(GTK_BOX(hdr), popout_btn);
    win->popout_btn = popout_btn;

    gtk_box_append(GTK_BOX(box), hdr);
  }

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 60);
  gtk_widget_set_vexpand(scroll, TRUE);

  win->output_view = GTK_WIDGET(gtk_source_view_new());
  gtk_text_view_set_editable(GTK_TEXT_VIEW(win->output_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->output_view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->output_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->output_view), 2);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->output_view), 4);
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(win->output_view),
                                        TRUE);
  gtk_widget_add_css_class(win->output_view, "monospace");
  gtk_widget_add_css_class(win->output_view, "output-font");
  gtk_source_view_set_show_line_marks(GTK_SOURCE_VIEW(win->output_view), TRUE);

  color =
      runtime_config_get_string(win->config, "mark_bg_color", MARK_BG_COLOR);
  hex_to_rgba(color, &c);
  {
    GtkSourceMarkAttributes *attrs;

    attrs = gtk_source_mark_attributes_new();
    gtk_source_mark_attributes_set_background(attrs, &c);
    gtk_source_mark_attributes_set_icon_name(attrs, "media-record-symbolic");
    gtk_source_view_set_mark_attributes(GTK_SOURCE_VIEW(win->output_view),
                                        "promptr-mark", attrs, 0);
  }

  {
    GtkGesture *gesture;

    gesture = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                               GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(win->output_view, GTK_EVENT_CONTROLLER(gesture));
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_gutter_click), win);
  }

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), win->output_view);
  win->output_scroll = scroll;
  gtk_box_append(GTK_BOX(box), scroll);

  return box;
}

static GtkWidget *create_marked_row(AppWindow *win) {
  GtkWidget *row;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);

  win->marked_label = gtk_label_new("Marked lines: none");
  gtk_widget_set_margin_start(win->marked_label, 48);
  gtk_label_set_xalign(GTK_LABEL(win->marked_label), 0.0f);
  gtk_widget_set_margin_top(win->marked_label, 4);
  gtk_widget_set_margin_bottom(win->marked_label, 4);
  gtk_box_append(GTK_BOX(row), win->marked_label);

  return row;
}

static GtkWidget *create_action_row(AppWindow *win) {
  GtkWidget *row;

  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  win->copy_btn = gtk_button_new_with_label("Copy Marked Lines");
  g_object_ref_sink(win->copy_btn);
  gtk_widget_set_sensitive(win->copy_btn, FALSE);
  g_signal_connect_swapped(win->copy_btn, "clicked", G_CALLBACK(on_copy), win);
  gtk_box_append(GTK_BOX(row), win->copy_btn);

  win->close_btn = gtk_button_new_with_label("Close");
  gtk_widget_set_margin_start(win->close_btn, 12);
  g_signal_connect_swapped(win->close_btn, "clicked", G_CALLBACK(on_close),
                           win);
  gtk_box_append(GTK_BOX(row), win->close_btn);

  win->quit_btn = gtk_button_new_with_label("Close & Quit");
  gtk_widget_add_css_class(win->quit_btn, "destructive-action");
  g_signal_connect_swapped(win->quit_btn, "clicked", G_CALLBACK(on_quit), win);
  gtk_box_append(GTK_BOX(row), win->quit_btn);

  return row;
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

static void setup_tooltips(AppWindow *win) {
  g_autofree char *t_copy = accel_to_human(
      runtime_config_get_string(win->config, "kb_copy_marked", KB_COPY_MARKED));
  g_autofree char *t_close = accel_to_human(
      runtime_config_get_string(win->config, "kb_close", KB_CLOSE));
  g_autofree char *t_quit = accel_to_human(
      runtime_config_get_string(win->config, "kb_quit", KB_QUIT));
  g_autofree char *t_log =
      accel_to_human(runtime_config_get_string(win->config, "kb_log", KB_LOG));
  g_autofree char *t_shortcuts = accel_to_human(
      runtime_config_get_string(win->config, "kb_shortcuts", KB_SHORTCUTS));
  g_autofree char *t_submit = accel_to_human(
      runtime_config_get_string(win->config, "kb_submit", KB_SUBMIT));
  g_autofree char *t_cancel = accel_to_human(
      runtime_config_get_string(win->config, "kb_cancel", KB_CANCEL));
  char *tip;

  tip = g_strdup_printf("Copy Marked Lines: %s", t_copy);
  gtk_widget_set_tooltip_text(win->copy_btn, tip);
  g_free(tip);
  tip = g_strdup_printf("Close: %s", t_close);
  gtk_widget_set_tooltip_text(win->close_btn, tip);
  g_free(tip);
  tip = g_strdup_printf("Close & Quit: %s", t_quit);
  gtk_widget_set_tooltip_text(win->quit_btn, tip);
  g_free(tip);
  tip = g_strdup_printf("Log: %s", t_log);
  gtk_widget_set_tooltip_text(win->log_btn, tip);
  g_free(tip);
  tip = g_strdup_printf("Shortcuts: %s", t_shortcuts);
  gtk_widget_set_tooltip_text(win->shortcuts_btn, tip);
  g_free(tip);

  tip = g_strdup_printf("Log: %s", t_log);
  gtk_widget_set_tooltip_text(win->log_btn_top, tip);
  g_free(tip);
  tip = g_strdup_printf("Shortcuts: %s", t_shortcuts);
  gtk_widget_set_tooltip_text(win->shortcuts_btn_top, tip);
  g_free(tip);

  tip = g_strdup_printf("Submit: %s", t_submit);
  gtk_widget_set_tooltip_text(win->submit_btn, tip);
  g_free(tip);
  if (strlen(t_cancel) > 0) {
    tip = g_strdup_printf("Cancel: ESC, ESC / %s", t_cancel);
  } else {
    tip = g_strdup("Cancel: ESC, ESC");
  }
  gtk_widget_set_tooltip_text(win->cancel_btn, tip);
  g_free(tip);

  gtk_widget_set_tooltip_text(win->follow_up_check,
                              "Continue last session instead of starting new");

  tip = g_strdup_printf("Copy marked lines: %s", t_copy);
  status_bar_on_hover(win->copy_btn, win, tip);
  g_free(tip);
  tip = g_strdup_printf("Close window: %s", t_close);
  status_bar_on_hover(win->close_btn, win, tip);
  g_free(tip);
  tip = g_strdup_printf("Close & quit: %s", t_quit);
  status_bar_on_hover(win->quit_btn, win, tip);
  g_free(tip);
  tip = g_strdup_printf("Open session log: %s", t_log);
  status_bar_on_hover(win->log_btn, win, tip);
  g_free(tip);
  tip = g_strdup_printf("Open shortcuts: %s", t_shortcuts);
  status_bar_on_hover(win->shortcuts_btn, win, tip);
  g_free(tip);

  tip = g_strdup_printf("Open session log: %s", t_log);
  status_bar_on_hover(win->log_btn_top, win, tip);
  g_free(tip);
  tip = g_strdup_printf("Open shortcuts: %s", t_shortcuts);
  status_bar_on_hover(win->shortcuts_btn_top, win, tip);
  g_free(tip);

  tip = g_strdup_printf("Submit prompt: %s", t_submit);
  status_bar_on_hover(win->submit_btn, win, tip);
  g_free(tip);
  if (strlen(t_cancel) > 0) {
    tip = g_strdup_printf("Interrupt running query: ESC, ESC / %s", t_cancel);
  } else {
    tip = g_strdup("Interrupt running query: ESC, ESC");
  }
  status_bar_on_hover(win->cancel_btn, win, tip);
  g_free(tip);

  status_bar_on_hover(win->follow_up_check, win,
                      "Submit to continue the previous session");
  status_bar_on_hover(
      win->prompt_view, win,
      "Type your prompt.  Ctrl+Enter to submit, Enter for newline.");
  status_bar_on_hover(
      win->output_view, win,
      "Click gutter to mark lines.  Ctrl+Shift+C to copy marked lines.");
}

static void _box_remove_all(GtkBox *box) {
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
    gtk_box_remove(box, child);
}

static void apply_layout(AppWindow *win) {
  _box_remove_all(GTK_BOX(win->pane_left));
  _box_remove_all(GTK_BOX(win->pane_right));
  _box_remove_all(GTK_BOX(win->layout_popped));

  gtk_widget_set_visible(win->popout_btn, !win->output_popped);
  gtk_widget_set_visible(win->prompt_btns,
                         !win->output_popped && win->layout_mode == 1);
  gtk_widget_set_visible(win->agent_btns,
                         !win->output_popped && win->layout_mode == 0);

  if (win->output_popped) {
    GtkWidget *spacer;

    gtk_box_append(GTK_BOX(win->layout_popped), win->prompt_section);
    gtk_box_append(GTK_BOX(win->layout_popped), win->fu_row);
    gtk_box_append(GTK_BOX(win->layout_popped), win->agent_row);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(win->layout_popped), spacer);

    gtk_box_append(GTK_BOX(win->layout_popped), win->action_row);

    gtk_stack_set_visible_child_name(GTK_STACK(win->content_stack), "popped");
  } else {
    GtkOrientation orientation;

    orientation = win->layout_mode == 0 ? GTK_ORIENTATION_VERTICAL
                                        : GTK_ORIENTATION_HORIZONTAL;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(win->layout_paned),
                                   orientation);

    gtk_paned_set_start_child(GTK_PANED(win->layout_paned), win->pane_left);
    gtk_paned_set_end_child(GTK_PANED(win->layout_paned), win->pane_right);

    gtk_widget_set_margin_start(win->pane_left, 12);
    gtk_widget_set_margin_end(win->pane_left,
                              orientation == GTK_ORIENTATION_VERTICAL ? 12 : 4);
    gtk_widget_set_margin_top(win->pane_left, 12);

    gtk_widget_set_margin_start(win->pane_right, 12);
    gtk_widget_set_margin_end(win->pane_right, 12);
    gtk_widget_set_margin_top(win->pane_right, 12);

    gtk_widget_set_size_request(win->pane_left, 200, 160);
    gtk_widget_set_size_request(win->pane_right, 200, 200);

    gtk_box_append(GTK_BOX(win->pane_left), win->prompt_section);
    gtk_box_append(GTK_BOX(win->pane_left), win->fu_row);
    gtk_box_append(GTK_BOX(win->pane_left), win->agent_row);

    gtk_box_append(GTK_BOX(win->pane_right), win->output_section);
    gtk_box_append(GTK_BOX(win->pane_right), win->marked_row);
    gtk_box_append(GTK_BOX(win->pane_right), win->action_row);

    gtk_paned_set_shrink_start_child(GTK_PANED(win->layout_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(win->layout_paned), FALSE);

    gtk_paned_set_position(GTK_PANED(win->layout_paned),
                           orientation == GTK_ORIENTATION_VERTICAL ? 350 : 450);

    gtk_stack_set_visible_child_name(GTK_STACK(win->content_stack), "paned");
    gtk_widget_grab_focus(win->prompt_view);
  }
}

static void toggle_popout(AppWindow *win) {
  if (!win->output_popped) {
    GtkWidget *popup, *popup_box, *btn, *crow;
    struct PopupEscCtx *ctx;
    GtkEventController *pctrl;

    win->output_popped = TRUE;
    apply_layout(win);

    gtk_box_remove(GTK_BOX(win->action_row), win->copy_btn);

    popup = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(popup), "Promptr — Output");
    gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(win->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(popup), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(popup), 700, 400);

    ctx = g_new(struct PopupEscCtx, 1);
    ctx->win = win;
    ctx->close_fn = toggle_popout;
    pctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(popup, pctrl);
    g_signal_connect_data(pctrl, "key-pressed", G_CALLBACK(on_popup_esc), ctx,
                          (GClosureNotify)esc_ctx_free, 0);

    popup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(popup_box, 12);
    gtk_widget_set_margin_end(popup_box, 12);
    gtk_widget_set_margin_top(popup_box, 8);
    gtk_widget_set_margin_bottom(popup_box, 8);
    gtk_window_set_child(GTK_WINDOW(popup), popup_box);

    gtk_box_append(GTK_BOX(popup_box), win->output_section);
    gtk_box_append(GTK_BOX(popup_box), win->marked_row);

    crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(crow), win->copy_btn);
    btn = gtk_button_new_with_label("Dock");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(toggle_popout), win);
    gtk_box_append(GTK_BOX(crow), btn);
    gtk_box_append(GTK_BOX(popup_box), crow);

    win->popout_window = popup;
    gtk_window_present(GTK_WINDOW(popup));
  } else {
    GtkWidget *popup, *popup_box, *crow;

    popup = win->popout_window;
    if (popup != NULL && GTK_IS_WINDOW(popup)) {
      popup_box = gtk_window_get_child(GTK_WINDOW(popup));
      if (popup_box != NULL && GTK_IS_BOX(popup_box)) {
        gtk_box_remove(GTK_BOX(popup_box), win->output_section);
        gtk_box_remove(GTK_BOX(popup_box), win->marked_row);

        crow = gtk_widget_get_parent(win->copy_btn);
        if (crow != NULL && GTK_IS_BOX(crow))
          gtk_box_remove(GTK_BOX(crow), win->copy_btn);
      }
      gtk_window_destroy(GTK_WINDOW(popup));
    }
    win->popout_window = NULL;
    win->output_popped = FALSE;

    gtk_box_prepend(GTK_BOX(win->action_row), win->copy_btn);
    apply_layout(win);
  }
}

AppWindow *app_window_new(GtkApplication *app) {
  AppWindow *win;
  GtkWidget *main_box;
  GtkWidget *status_bar_widget;
  g_autofree char *kb;

  win = g_new0(AppWindow, 1);

  win->app = app;
  win->config = runtime_config_load();
  win->marked_lines_str = runtime_config_get_string(win->config, "marked_lines",
                                                    DEFAULT_MARKED_LINES_STR);

  {
    g_autofree char *raw;

    raw =
        runtime_config_get_string(win->config, "opencode_path", OPENCODE_PATH);
    if (g_str_has_prefix(raw, "~/"))
      win->opencode_bin = g_build_filename(g_get_home_dir(), raw + 2, NULL);
    else
      win->opencode_bin = g_strdup(raw);
  }
  {
    const char *data_dir;
    g_autofree char *log_dir = NULL;
    g_autofree char *log_path = NULL;
    char timestr[64];
    time_t now;
    struct tm *tm;

    data_dir = g_get_user_data_dir();
    log_dir = g_build_filename(data_dir, "promptr", "logs", NULL);
    g_mkdir_with_parents(log_dir, 0700);

    now = time(NULL);
    tm = localtime(&now);
    strftime(timestr, sizeof(timestr), "promptr-%Y%m%dT%H%M%S.log", tm);
    log_path = g_build_filename(log_dir, timestr, NULL);
    win->log_file = fopen(log_path, "w");
    if (win->log_file != NULL) {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
      fprintf(win->log_file, "Promptr v" VERSION ": session %s\n", timestr);
      fflush(win->log_file);
    }
  }
  {
    int prompt_fs, output_fs;

    prompt_fs = runtime_config_get_int(win->config, "prompt_font_size",
                                       PROMPT_FONT_SIZE_DEFAULT);
    output_fs = runtime_config_get_int(win->config, "output_font_size",
                                       OUTPUT_FONT_SIZE_DEFAULT);
    load_css(prompt_fs, output_fs);
  }

  kb = runtime_config_get_string(win->config, "kb_focus_prompt",
                                 KB_FOCUS_PROMPT);
  gtk_accelerator_parse(kb, &win->kb_focus_keyval, &win->kb_focus_mods);
  kb = runtime_config_get_string(win->config, "kb_copy_marked", KB_COPY_MARKED);
  gtk_accelerator_parse(kb, &win->kb_copy_keyval, &win->kb_copy_mods);
  kb = runtime_config_get_string(win->config, "kb_close", KB_CLOSE);
  gtk_accelerator_parse(kb, &win->kb_close_keyval, &win->kb_close_mods);
  kb = runtime_config_get_string(win->config, "kb_quit", KB_QUIT);
  gtk_accelerator_parse(kb, &win->kb_quit_keyval, &win->kb_quit_mods);
  kb = runtime_config_get_string(win->config, "kb_shortcuts", KB_SHORTCUTS);
  gtk_accelerator_parse(kb, &win->kb_shortcuts_keyval, &win->kb_shortcuts_mods);
  kb = runtime_config_get_string(win->config, "kb_log", KB_LOG);
  gtk_accelerator_parse(kb, &win->kb_log_keyval, &win->kb_log_mods);
  kb = runtime_config_get_string(win->config, "kb_submit", KB_SUBMIT);
  gtk_accelerator_parse(kb, &win->kb_submit_keyval, &win->kb_submit_mods);
  kb = runtime_config_get_string(win->config, "kb_cancel", KB_CANCEL);
  gtk_accelerator_parse(kb, &win->kb_cancel_keyval, &win->kb_cancel_mods);
  kb = runtime_config_get_string(win->config, "kb_layout", KB_LAYOUT);
  gtk_accelerator_parse(kb, &win->kb_layout_keyval, &win->kb_layout_mods);
  kb = runtime_config_get_string(win->config, "kb_popout", KB_POPOUT);
  gtk_accelerator_parse(kb, &win->kb_popout_keyval, &win->kb_popout_mods);

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
    };
    gboolean conflict = FALSE;
    int n = (int)G_N_ELEMENTS(binds);

    for (int i = 0; i < n && !conflict; i++)
      for (int j = i + 1; j < n; j++)
        if (binds[i].keyval == binds[j].keyval &&
            binds[i].mods == binds[j].mods) {
          g_warning("Shortcut conflict: \"%s\" and \"%s\" both resolve to "
                    "the same key combination",
                    binds[i].name, binds[j].name);
          conflict = TRUE;
          break;
        }
  }

  {
    g_autofree char *layout_mode;

    layout_mode =
        runtime_config_get_string(win->config, "layout", LAYOUT_DEFAULT);
    win->layout_mode = (g_strcmp0(layout_mode, "vertical") == 0) ? 1 : 0;
  }

  win->window = gtk_window_new();
  gtk_window_set_application(GTK_WINDOW(win->window), app);
  {
    GtkSettings *settings;

    settings = gtk_widget_get_settings(win->window);
    g_object_set(settings, "gtk-cursor-aspect-ratio", 0.08, NULL);
  }
  gtk_window_set_title(GTK_WINDOW(win->window), "Promptr");
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
    gtk_layer_set_namespace(GTK_WINDOW(win->window), "promptr");
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

  win->prompt_section = create_prompt_section(win);
  win->fu_row = create_follow_up_row(win);
  win->agent_row = create_agent_row(win);
  win->output_section = create_output_section(win);
  win->marked_row = create_marked_row(win);
  win->action_row = create_action_row(win);

  g_object_ref_sink(win->prompt_section);
  g_object_ref_sink(win->fu_row);
  g_object_ref_sink(win->agent_row);
  g_object_ref_sink(win->output_section);
  g_object_ref_sink(win->marked_row);
  g_object_ref_sink(win->action_row);

  win->cmd_label = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(win->cmd_label), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->cmd_label), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->cmd_label),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->cmd_label), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->cmd_label), 4);
  gtk_widget_set_hexpand(win->cmd_label, TRUE);
  gtk_widget_add_css_class(win->cmd_label, "monospace");

  log_append(win, "session → started");

  status_bar_widget = create_status_bar(win);
  gtk_box_append(GTK_BOX(main_box), status_bar_widget);

  {
    win->content_stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(win->content_stack), FALSE);
    gtk_widget_set_vexpand(win->content_stack, TRUE);
    gtk_box_prepend(GTK_BOX(main_box), win->content_stack);

    win->layout_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_stack_add_named(GTK_STACK(win->content_stack), win->layout_paned,
                        "paned");

    win->pane_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    win->pane_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_object_ref_sink(win->pane_left);
    g_object_ref_sink(win->pane_right);

    win->layout_popped = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(win->layout_popped, 12);
    gtk_widget_set_margin_end(win->layout_popped, 12);
    gtk_widget_set_margin_top(win->layout_popped, 12);
    gtk_widget_set_vexpand(win->layout_popped, TRUE);
    gtk_stack_add_named(GTK_STACK(win->content_stack), win->layout_popped,
                        "popped");
  }

  setup_tooltips(win);
  apply_layout(win);

  app_window_restore_state(win);
  update_cmd_preview(win);

  return win;
}

void app_window_show(AppWindow *win) {
  gtk_window_present(GTK_WINDOW(win->window));
  set_prompt_focused(win);
}

void app_window_present(AppWindow *win) {
  gtk_window_present(GTK_WINDOW(win->window));
  set_prompt_focused(win);
}

static void remove_temp_dirs(AppWindow *win) {
  GSList *l;
  GDir *dir;
  const char *name;
  g_autofree char *path = NULL;

  if (win->temp_dirs == NULL)
    return;

  for (l = win->temp_dirs; l != NULL; l = l->next) {
    dir = g_dir_open((const char *)l->data, 0, NULL);
    if (dir != NULL) {
      while ((name = g_dir_read_name(dir)) != NULL) {
        path = g_build_filename((const char *)l->data, name, NULL);
        unlink(path);
      }
      g_dir_close(dir);
    }
    rmdir((const char *)l->data);
    g_free(l->data);
  }
  g_slist_free(win->temp_dirs);
  win->temp_dirs = NULL;
}

void app_window_close_and_quit(AppWindow *win) {
  if (win == NULL)
    return;
  remove_temp_dirs(win);
  if (win->subprocess != NULL) {
    command_cancel(win);
  }
  if (win->cancellable != NULL) {
    g_cancellable_cancel(win->cancellable);
  }
  if (win->log_popup != NULL)
    gtk_window_destroy(GTK_WINDOW(win->log_popup));
  if (win->log_file != NULL) {
    fclose(win->log_file);
    win->log_file = NULL;
  }
  win->destroyed = TRUE;
  gtk_window_destroy(GTK_WINDOW(win->window));
}

void app_window_free(gpointer data) {
  AppWindow *win = data;

  if (win == NULL)
    return;
  remove_temp_dirs(win);
  if (win->subprocess != NULL) {
    command_cancel(win);
  }
  g_clear_object(&win->cancellable);
  g_free(win->cmd_string);
  g_free(win->marked_lines_str);
  g_free(win->opencode_bin);
  g_free(win->last_tmpdir);
  g_list_free_full(win->qa_history, g_free);
  g_free(win->last_query);
  g_free(win->last_output);
  runtime_config_free(win->config);
  if (win->log_file != NULL)
    fclose(win->log_file);
  g_free(win);
}

/* ── state management ──────────────────────────────────────────── */

static void set_load_state_common(AppWindow *win, gboolean loading) {
  gtk_widget_set_sensitive(win->prompt_view, !loading);
  gtk_widget_set_sensitive(win->agent_dropdown, !loading);
  gtk_widget_set_sensitive(win->model_dropdown, !loading);
  gtk_widget_set_sensitive(win->follow_up_check, !loading);
  gtk_widget_set_sensitive(win->submit_btn, !loading);
  gtk_widget_set_visible(win->cancel_btn, loading);
  gtk_widget_set_sensitive(win->cancel_btn, loading);
  gtk_spinner_set_spinning(GTK_SPINNER(win->spinner), loading);
  gtk_widget_set_visible(win->spinner, loading);
}

static void set_loading_state(AppWindow *win, const char *cmd) {
  win->state = STATE_LOADING;
  set_load_state_common(win, TRUE);

  log_append(win, "submit → %s%s", win->follow_up_active ? "[follow-up] " : "",
             cmd);

  gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Running...");

  set_status_text(win, "Running Query: ESC to interrupt");

  update_submit_sensitivity(win);
}

static void set_finished_state(AppWindow *win, char *cmd, gint64 elapsed,
                               const char *output) {
  gint64 ms;
  int lines;

  disarm_escape(win);
  set_load_state_common(win, FALSE);
  win->state = STATE_FINISHED;

  set_status_text(win, "Ready");

  ms = elapsed / 1000;
  if (output != NULL && output[0] != '\0') {
    GtkTextBuffer *buf;
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
    if (win->follow_up_active) {
      GString *display = g_string_new(output);
      GList *l;

      for (l = win->qa_history; l != NULL; l = l->next)
        g_string_append_printf(display, "\n---\n%s", (char *)l->data);
      {
        g_autofree char *combined = g_string_free(display, FALSE);
        gtk_text_buffer_set_text(buf, combined, -1);
      }
      lines = gtk_text_buffer_get_line_count(buf);
    } else {
      gtk_text_buffer_set_text(buf, output, -1);
      lines = gtk_text_buffer_get_line_count(buf);
      if (!win->defaults_applied) {
        apply_default_marks(win, buf);
        win->defaults_applied = TRUE;
      }
    }
    g_free(win->last_output);
    win->last_output = g_strdup(output);
    update_marked_label(win);
    gtk_widget_set_sensitive(win->copy_btn, TRUE);
  } else {
    GtkTextBuffer *buf;
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
    if (!win->follow_up_active)
      gtk_text_buffer_set_text(buf, "", -1);
    lines = gtk_text_buffer_get_line_count(buf);
    gtk_widget_set_sensitive(win->copy_btn, lines > 0);
  }

  log_append(win, "finished → Took %" G_GINT64_FORMAT "ms. %d lines.", ms,
             lines);
  g_free(cmd);

  gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Submit");
  update_submit_sensitivity(win);
  set_prompt_focused(win);
}

static void set_canceled_state(AppWindow *win, char *cmd) {
  disarm_escape(win);
  set_load_state_common(win, FALSE);
  win->state = STATE_CANCELED;

  set_status_text(win, "Interrupted");
  g_timeout_add(2000, (GSourceFunc)status_pop_cb, win->status_bar);

  log_append(win, "cancel → Cancelled.");
  g_free(cmd);

  gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Submit");
  update_submit_sensitivity(win);
  set_prompt_focused(win);
}

static void set_errored_state(AppWindow *win, char *cmd,
                              const char *stderr_output) {
  GtkTextBuffer *buf;
  g_autofree char *err_summary = NULL;

  disarm_escape(win);
  set_load_state_common(win, FALSE);
  win->state = STATE_FINISHED;

  set_status_text(win, "Ready");

  if (stderr_output != NULL && stderr_output[0] != '\0') {
    const char *nl;
    nl = strchr(stderr_output, '\n');
    if (nl != NULL && nl - stderr_output < 120)
      err_summary = g_strndup(stderr_output, nl - stderr_output);
    else
      err_summary = g_strndup(stderr_output, 120);
  }
  log_append(win, "errored → %s",
             err_summary != NULL ? err_summary : "unknown error");
  g_free(cmd);

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
  if (stderr_output != NULL)
    gtk_text_buffer_set_text(buf, stderr_output, -1);
  else
    gtk_text_buffer_set_text(buf, "", -1);
  gtk_widget_set_sensitive(win->copy_btn,
                           stderr_output != NULL && stderr_output[0] != '\0');
  update_marked_label(win);

  gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Submit");
  update_submit_sensitivity(win);
  set_prompt_focused(win);
}

/* ── submit ────────────────────────────────────────────────────── */

static void on_submit(AppWindow *win) {
  char *query, *agent, *model;
  GString *display;
  char *cmd_display;
  GtkTextBuffer *outbuf;
  g_autofree char *tmpdir = NULL;
  gboolean is_follow_up;
  GError *err = NULL;

  if (win->subprocess != NULL)
    return;

  query = get_trimmed_text(win->prompt_view);
  if (query == NULL || query[0] == '\0') {
    g_free(query);
    return;
  }

  agent = get_selected_text(win->agent_dropdown);
  model = get_selected_text(win->model_dropdown);

  win->follow_up_active = win->follow_up;

  if (win->follow_up_active && win->last_tmpdir != NULL) {
    is_follow_up = TRUE;
  } else {
    is_follow_up = FALSE;
    win->follow_up_active = FALSE;

    tmpdir = g_dir_make_tmp("promptr-XXXXXX", &err);
    if (tmpdir == NULL) {
      g_warning("Failed to create temp dir: %s", err->message);
      g_clear_error(&err);
    } else {
      g_free(win->last_tmpdir);
      win->last_tmpdir = g_strdup(tmpdir);
      gtk_widget_set_sensitive(win->follow_up_check, TRUE);
    }
  }

  if (is_follow_up) {
    if (win->last_output != NULL) {
      win->follow_up_turn++;
      char *block = g_strdup_printf("Q.%d: %s\nA: %s", win->follow_up_turn,
                                    win->last_query, win->last_output);
      win->qa_history = g_list_prepend(win->qa_history, block);
    }
  } else {
    g_list_free_full(win->qa_history, g_free);
    win->qa_history = NULL;
    win->follow_up_turn = 0;
    g_free(win->last_output);
    win->last_output = NULL;
  }
  g_free(win->last_query);
  win->last_query = g_strdup(query);

  if (is_follow_up)
    tmpdir = g_strdup(win->last_tmpdir);

  display = g_string_new(win->opencode_bin);
  g_string_append(display, " run");
  if (is_follow_up)
    g_string_append(display, " --continue");
  if (tmpdir != NULL)
    g_string_append_printf(display, " --dir %s", tmpdir);
  if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
    g_string_append_printf(display, " --model %s", model);
  if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
    g_string_append_printf(display, " --agent %s", agent);
  g_string_append_printf(display, " %s", query);
  cmd_display = g_string_free(display, FALSE);

  g_free(win->cmd_string);
  win->cmd_string = cmd_display;

  outbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
  if (!is_follow_up)
    gtk_text_buffer_set_text(outbuf, "", -1);
  gtk_widget_set_sensitive(win->copy_btn, FALSE);

  command_execute(win, model, agent, query, tmpdir, win->opencode_bin,
                  is_follow_up, command_finished_cb);

  if (win->subprocess != NULL && tmpdir != NULL && !is_follow_up)
    win->temp_dirs = g_slist_prepend(win->temp_dirs, g_strdup(tmpdir));

  if (win->subprocess != NULL)
    set_loading_state(win, cmd_display);

  g_free(query);
  g_free(agent);
  g_free(model);
}

static void command_finished_cb(AppWindow *win, const char *output,
                                const char *stderr_output, gint64 elapsed,
                                int exit_code, gboolean exited_cleanly) {
  char *cmd;

  cmd = win->cmd_string;
  win->cmd_string = NULL;

  if (win->destroyed) {
    g_free(cmd);
    return;
  }

  if (!exited_cleanly) {
    set_canceled_state(win, cmd);
    return;
  }

  if (exit_code != 0) {
    set_errored_state(win, cmd, stderr_output);
    return;
  }

  set_finished_state(win, cmd, elapsed, output);
}

/* ── cancel ────────────────────────────────────────────────────── */

static void on_cancel(AppWindow *win) {
  if (win->subprocess == NULL)
    return;
  log_append(win, "cancel → user requested");
  command_cancel(win);
}

static void on_follow_up_toggled(AppWindow *win) {
  win->follow_up =
      gtk_check_button_get_active(GTK_CHECK_BUTTON(win->follow_up_check));
  update_cmd_preview(win);
}

static gboolean esc_arm_timeout_cb(gpointer data) {
  AppWindow *win = data;

  win->esc_armed = FALSE;
  win->esc_reset_timeout = 0;
  if (win->subprocess != NULL)
    set_status_text(win, "Running Query: ESC to interrupt");
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
  gtk_label_set_text(GTK_LABEL(data), "Ready");
  return G_SOURCE_REMOVE;
}

static void set_status_text(AppWindow *win, const char *text) {
  gtk_label_set_text(GTK_LABEL(win->status_bar), text != NULL ? text : "");
}

static void on_copy(AppWindow *win) {
  char *text;
  GdkClipboard *clipboard;
  size_t nlines;
  g_autofree char *msg = NULL;

  text = get_marked_text(win);
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
  g_timeout_add_seconds(2, status_pop_cb, win->status_bar);

  log_append(win, "copy → %zu chars to clipboard", strlen(text));
  g_free(text);
}

/* ── close / quit ──────────────────────────────────────────────── */

static void on_close(AppWindow *win) {
  if (win->subprocess != NULL)
    command_cancel(win);
  gtk_widget_set_visible(win->window, FALSE);
}

static void on_quit(AppWindow *win) {
  g_application_quit(G_APPLICATION(win->app));
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
  AppWindow *win = user_data;

  (void)window;
  if (win->subprocess != NULL)
    command_cancel(win);
  gtk_widget_set_visible(win->window, FALSE);
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
    struct tm *tm;

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
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), win->cmd_label);
    gtk_box_append(GTK_BOX(box), scroll);

    now = time(NULL);
    tm = localtime(&now);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
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
      ctx->win = win;
      ctx->close_fn = on_log_close;

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
    gtk_window_set_default_size(GTK_WINDOW(popup), 460, 360);
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

    struct {
      const char *shortcut;
      const char *desc;
    } rows[] = {
        {NULL, "Focus prompt input"}, {NULL, "Copy marked lines"},
        {NULL, "Close window"},       {NULL, "Close & quit"},
        {NULL, "Open session log"},   {NULL, "Open this shortcuts window"},
        {NULL, "Submit prompt"},      {NULL, "Cancel"},
    };
    const char *hardcoded_shortcuts[] = {
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, "ESC, ESC",
    };

    int n_config = 7;

    rows[0].shortcut = accel_to_human(runtime_config_get_string(
        win->config, "kb_focus_prompt", KB_FOCUS_PROMPT));
    rows[1].shortcut = accel_to_human(runtime_config_get_string(
        win->config, "kb_copy_marked", KB_COPY_MARKED));
    rows[2].shortcut = accel_to_human(
        runtime_config_get_string(win->config, "kb_close", KB_CLOSE));
    rows[3].shortcut = accel_to_human(
        runtime_config_get_string(win->config, "kb_quit", KB_QUIT));
    rows[4].shortcut = accel_to_human(
        runtime_config_get_string(win->config, "kb_log", KB_LOG));
    rows[5].shortcut = accel_to_human(
        runtime_config_get_string(win->config, "kb_shortcuts", KB_SHORTCUTS));
    rows[6].shortcut = accel_to_human(
        runtime_config_get_string(win->config, "kb_submit", KB_SUBMIT));

    for (int i = n_config; i < (int)G_N_ELEMENTS(rows); i++)
      rows[i].shortcut = hardcoded_shortcuts[i];

    for (int i = 0; i < (int)G_N_ELEMENTS(rows); i++) {
      GtkWidget *k, *d;
      const char *row_class;
      int row = i + 1;

      row_class = (i % 2 == 0) ? NULL : "shortcuts-row-even";
      k = cell_box(kbd_label(rows[i].shortcut), row_class, FALSE);
      d = cell_box(desc_label(rows[i].desc), row_class, TRUE);
      gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
      gtk_grid_attach(GTK_GRID(grid), d, 1, row, 1, 1);
    }

    for (int i = 0; i < (int)G_N_ELEMENTS(rows); i++)
      if (hardcoded_shortcuts[i] == NULL)
        g_free((char *)rows[i].shortcut);

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
      ctx->win = win;
      ctx->close_fn = on_shortcuts_close;

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

/* ── key handling ──────────────────────────────────────────────── */

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, AppWindow *win) {
  GdkModifierType mods;

  (void)controller;
  (void)keycode;

  mods = state &
         (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);

  if (keyval == GDK_KEY_Escape) {
    if (win->subprocess != NULL) {
      if (win->esc_armed) {
        disarm_escape(win);
        on_cancel(win);
        return GDK_EVENT_STOP;
      }
      win->esc_armed = TRUE;
      win->esc_reset_timeout = g_timeout_add(2000, esc_arm_timeout_cb, win);
      set_status_text(win, "Press ESC again to interrupt");
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if (win->kb_cancel_keyval != 0 &&
      gdk_keyval_to_lower(keyval) == win->kb_cancel_keyval &&
      mods == win->kb_cancel_mods) {
    if (win->subprocess != NULL) {
      disarm_escape(win);
      on_cancel(win);
      return GDK_EVENT_STOP;
    }
  }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    if (win->kb_submit_mods != 0 && mods == win->kb_submit_mods) {
      if (gtk_widget_is_sensitive(win->submit_btn) && win->subprocess == NULL)
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

  (void)controller;
  (void)keycode;

  keyval = gdk_keyval_to_lower(keyval);
  mods = state &
         (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);

  if (keyval == GDK_KEY_Escape) {
    if (win->subprocess != NULL) {
      if (win->esc_armed) {
        disarm_escape(win);
        on_cancel(win);
        return GDK_EVENT_STOP;
      }
      win->esc_armed = TRUE;
      win->esc_reset_timeout = g_timeout_add(2000, esc_arm_timeout_cb, win);
      set_status_text(win, "Press ESC again to interrupt");
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if (win->kb_cancel_keyval != 0 && keyval == win->kb_cancel_keyval &&
      mods == win->kb_cancel_mods) {
    if (win->subprocess != NULL) {
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
    if (gtk_widget_is_sensitive(win->copy_btn))
      on_copy(win);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_focus_keyval && mods == win->kb_focus_mods) {
    gtk_widget_grab_focus(win->prompt_view);
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->prompt_view));
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gtk_text_buffer_select_range(buf, &start, &end);
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_layout_keyval && mods == win->kb_layout_mods) {
    if (!win->output_popped) {
      win->layout_mode = !win->layout_mode;
      apply_layout(win);
    }
    return GDK_EVENT_STOP;
  }

  if (keyval == win->kb_popout_keyval && mods == win->kb_popout_mods) {
    toggle_popout(win);
    return GDK_EVENT_STOP;
  }

  return GDK_EVENT_PROPAGATE;
}

/* ── prompt change ─────────────────────────────────────────────── */

static void on_prompt_changed(GtkTextBuffer *buffer, AppWindow *win) {
  (void)buffer;
  gtk_widget_set_visible(win->placeholder_label,
                         gtk_text_buffer_get_char_count(buffer) == 0);
  update_submit_sensitivity(win);
  update_cmd_preview(win);
}

static void update_submit_sensitivity(AppWindow *win) {
  gboolean enable;
  char *text;

  if (win->subprocess != NULL) {
    gtk_widget_set_sensitive(win->submit_btn, FALSE);
    return;
  }

  text = get_trimmed_text(win->prompt_view);
  enable = (text != NULL && text[0] != '\0');
  gtk_widget_set_sensitive(win->submit_btn, enable);
  g_free(text);
}

/* ── helpers ───────────────────────────────────────────────────── */

static char *get_trimmed_text(GtkWidget *text_view) {
  GtkTextBuffer *buf;
  GtkTextIter start, end;
  char *raw, *start_ptr, *end_ptr;
  size_t len;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
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

static char *get_selected_text(GtkWidget *dropdown) {
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

static char *accel_to_human(const char *accel) {
  GString *out;
  const char *p;

  out = g_string_new(NULL);
  p = accel != NULL ? accel : "";

  while (*p != '\0') {
    if (*p == '<') {
      p++;
      if (g_ascii_strncasecmp(p, "Control>", 8) == 0) {
        g_string_append(out, "ctrl+");
        p += 8;
      } else if (g_ascii_strncasecmp(p, "Shift>", 6) == 0) {
        g_string_append(out, "shift+");
        p += 6;
      } else if (g_ascii_strncasecmp(p, "Alt>", 4) == 0) {
        g_string_append(out, "alt+");
        p += 4;
      } else if (g_ascii_strncasecmp(p, "Super>", 6) == 0) {
        g_string_append(out, "super+");
        p += 6;
      } else {
        while (*p != '\0' && *p != '>')
          g_string_append_c(out, g_ascii_tolower(*p++));
        if (*p == '>')
          p++;
        g_string_append_c(out, '+');
      }
    } else {
      g_string_append_c(out, g_ascii_tolower(*p));
      p++;
    }
  }

  if (out->len > 0 && out->str[out->len - 1] == '+')
    g_string_truncate(out, out->len - 1);

  return g_string_free(out, FALSE);
}

struct HoverCtx {
  AppWindow *win;
  char *text;
};

static void hover_enter_cb(GtkEventControllerMotion *ctrl, double x, double y,
                           gpointer data) {
  struct HoverCtx *ctx = data;
  (void)ctrl;
  (void)x;
  (void)y;
  gtk_label_set_text(GTK_LABEL(ctx->win->status_bar), ctx->text);
}

static void hover_leave_cb(GtkEventControllerMotion *ctrl, gpointer data) {
  AppWindow *win = data;
  (void)ctrl;
  gtk_label_set_text(GTK_LABEL(win->status_bar), "Ready");
}

static void hover_ctx_free(gpointer data, GClosure *closure) {
  struct HoverCtx *ctx = data;
  (void)closure;
  g_free(ctx->text);
  g_free(ctx);
}

static void status_bar_on_hover(GtkWidget *widget, AppWindow *win,
                                const char *text) {
  GtkEventController *motion;
  struct HoverCtx *ctx;

  ctx = g_new(struct HoverCtx, 1);
  ctx->win = win;
  ctx->text = g_strdup(text);

  motion = gtk_event_controller_motion_new();
  g_signal_connect_data(motion, "enter", G_CALLBACK(hover_enter_cb), ctx,
                        (GClosureNotify)hover_ctx_free, 0);
  g_signal_connect(motion, "leave", G_CALLBACK(hover_leave_cb), win);

  gtk_widget_add_controller(widget, motion);
}

static void set_prompt_focused(AppWindow *win) {
  gtk_widget_grab_focus(win->prompt_view);
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

static void log_append(AppWindow *win, const char *fmt, ...) {
  GtkTextBuffer *buf;
  GtkTextIter end;
  GString *line;
  char timestr[64];
  time_t now;
  struct tm *tm;
  va_list args;
  g_autofree char *msg = NULL;

  now = time(NULL);
  tm = localtime(&now);
  strftime(timestr, sizeof(timestr), "[%H:%M:%S] ", tm);

  va_start(args, fmt);
  msg = g_strdup_vprintf(fmt, args);
  va_end(args);

  line = g_string_new(timestr);
  g_string_append(line, msg);
  g_string_append_c(line, '\n');

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->cmd_label));
  gtk_text_buffer_get_end_iter(buf, &end);
  gtk_text_buffer_insert(buf, &end, line->str, -1);

  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(win->cmd_label),
                               gtk_text_buffer_get_insert(buf), 0.0, FALSE, 0.0,
                               0.0);

  if (win->log_file != NULL) {
    fputs(line->str, win->log_file);
    fflush(win->log_file);
  }

  g_string_free(line, TRUE);
}

static gboolean hex_to_rgba(const char *hex, GdkRGBA *out) {
  unsigned int r, g, b;
  double a;

  if (hex == NULL || hex[0] != '#')
    return FALSE;

  if (sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
    a = 0.60;
  } else if (sscanf(hex + 1, "%2x%2x%2x%2x", &r, &g, &b, (unsigned int *)&a) ==
             4) {
    a /= 255.0;
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

static void on_dropdown_changed(GObject *self, GParamSpec *pspec,
                                AppWindow *win) {
  char *agent, *model;

  (void)self;
  (void)pspec;
  update_cmd_preview(win);

  agent = get_selected_text(win->agent_dropdown);
  model = get_selected_text(win->model_dropdown);
  state_save(model, agent);
  g_free(agent);
  g_free(model);
}

/* ── cmd preview ──────────────────────────────────────────────── */

static void update_cmd_preview(AppWindow *win) {
  char *query, *agent, *model;
  GString *display;

  if (win->state == STATE_LOADING)
    return;

  if (win->state == STATE_FINISHED || win->state == STATE_CANCELED)
    win->state = STATE_IDLE;

  query = get_trimmed_text(win->prompt_view);
  agent = get_selected_text(win->agent_dropdown);
  model = get_selected_text(win->model_dropdown);

  display = g_string_new(win->opencode_bin);
  g_string_append(display, " run");
  if (win->follow_up)
    g_string_append(display, " --continue --dir <tmp>");
  else
    g_string_append(display, " --dir <tmp>");
  if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
    g_string_append_printf(display, " --model %s", model);
  if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
    g_string_append_printf(display, " --agent %s", agent);
  if (query != NULL && query[0] != '\0')
    g_string_append_printf(display, " %s", query);
  else
    g_string_append(display, " <query>");

  g_string_free(display, TRUE);
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
  GtkTextBuffer *buf;
  GtkTextIter iter;
  int buf_x, buf_y;

  (void)gesture;
  (void)n_press;

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(win->output_view),
                                        GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y,
                                        &buf_x, &buf_y);

  if (buf_x >= 0)
    return;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(win->output_view), &iter,
                                     buf_x, buf_y);
  toggle_mark_at_iter(buf, &iter);
  update_marked_label(win);
}

static void update_marked_label(AppWindow *win) {
  GtkTextBuffer *buf;
  GtkTextIter iter;
  GString *label;
  int line, marked, total;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
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
    gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked lines: none");
    return;
  }

  if (marked == 0) {
    gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked lines: none");
    return;
  }

  if (marked == total) {
    gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked lines: all");
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

  gtk_label_set_text(GTK_LABEL(win->marked_label), label->str);
  g_string_free(label, TRUE);
}

static void apply_default_marks(AppWindow *win, GtkTextBuffer *buf) {
  GtkTextIter iter;
  int i, total, line;
  gboolean mark_all;
  g_autofree char **parts = NULL;

  if (g_strcmp0(win->marked_lines_str, "-1") == 0)
    return;

  mark_all = (g_strcmp0(win->marked_lines_str, "0") == 0);

  if (mark_all) {
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (!gtk_text_iter_is_end(&iter)) {
      gtk_source_buffer_create_source_mark(GTK_SOURCE_BUFFER(buf), NULL,
                                           "promptr-mark", &iter);
      gtk_text_iter_forward_line(&iter);
    }
    return;
  }

  parts = g_strsplit(win->marked_lines_str, ",", -1);
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

static char *get_marked_text(AppWindow *win) {
  GtkTextBuffer *buf;
  GString *result;
  GtkTextIter iter;
  int line;

  buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
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

/* ── state persistence ────────────────────────────────────────── */

void app_window_save_state(AppWindow *win) {
  char *agent, *model;

  agent = get_selected_text(win->agent_dropdown);
  model = get_selected_text(win->model_dropdown);
  state_save(model, agent);
  g_free(agent);
  g_free(model);
}

static void select_option_by_value(GtkWidget *dropdown, const char *value) {
  GListModel *model;
  guint i, n;

  if (value == NULL)
    return;

  model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
  n = g_list_model_get_n_items(model);
  for (i = 0; i < n; i++) {
    g_autoptr(GObject) item = g_list_model_get_item(model, i);
    const char *str;

    if (item == NULL)
      continue;
    str = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
    if (str != NULL && g_strcmp0(str, value) == 0) {
      gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
      return;
    }
  }
}

static void app_window_restore_state(AppWindow *win) {
  g_autofree char *model = NULL;
  g_autofree char *agent = NULL;

  if (!state_load(&model, &agent))
    return;

  if (model != NULL)
    select_option_by_value(win->model_dropdown, model);
  if (agent != NULL)
    select_option_by_value(win->agent_dropdown, agent);
}

/* ── CSS ───────────────────────────────────────────────────────── */

static void load_css(int prompt_font_size, int output_font_size) {
  static gboolean loaded = FALSE;
  GtkCssProvider *provider;
  GdkDisplay *display;
  GString *css;

  if (loaded)
    return;
  loaded = TRUE;

  css = g_string_new(
      "textview.monospace, label.monospace { font-family: monospace; }"
      ".kbd {"
      "  background-color: mix(@theme_bg_color, @theme_fg_color, 0.06);"
      "  border: 1px solid mix(@theme_bg_color, @theme_fg_color, 0.15);"
      "  border-radius: 4px;"
      "  padding: 2px 7px;"
      "  font-family: monospace;"
      "  font-size: 0.92em;"
      "}"
      "textview gutter {"
      "  background-color: mix(@theme_bg_color, @theme_fg_color, 0.05);"
      "  color: @theme_fg_color;"
      "}"
      ".status-bar {"
      "  background-color: mix(@theme_bg_color, @theme_fg_color, 0.05);"
      "}"
      ".shortcuts-grid {"
      "  border: 1px solid @borders;"
      "}"
      ".shortcuts-header {"
      "  background-color: alpha(currentColor, 0.06);"
      "  font-weight: bold;"
      "}"
      ".shortcuts-cell {"
      "  border-bottom: 1px solid @borders;"
      "  border-right: 1px solid @borders;"
      "}"
      ".shortcuts-cell-last {"
      "  border-right: none;"
      "}"
      ".shortcuts-row-even {"
      "  background-color: alpha(currentColor, 0.03);"
      "}"
      "dropdown button arrow {"
      "  -gtk-icon-source: -gtk-icontheme(\"pan-down-symbolic\");"
      "}");

  if (prompt_font_size > 0)
    g_string_append_printf(css,
                           "textview.prompt-font { font-size: %dpt;"
                           "caret-color: #00ab41; }",
                           prompt_font_size);
  else
    g_string_append(css, "textview.prompt-font { caret-color: #00ab41; }");
  if (output_font_size > 0)
    g_string_append_printf(css, "textview.output-font { font-size: %dpt; }",
                           output_font_size);

  provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css->str);
  g_string_free(css, TRUE);
  display = gdk_display_get_default();
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}
