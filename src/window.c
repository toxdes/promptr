#include "window.h"
#include "command.h"
#include "state.h"
#include "config.h"

#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <gtksourceview/gtksource.h>

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
static void update_submit_sensitivity(AppWindow *win);
static char *get_trimmed_text(GtkWidget *text_view);
static char *get_selected_text(GtkWidget *dropdown);
static void set_prompt_focused(AppWindow *win);

static void set_loading_state(AppWindow *win, const char *cmd);
static void set_finished_state(AppWindow *win, char *cmd, gint64 elapsed,
                               const char *output);
static void set_canceled_state(AppWindow *win, char *cmd);

static void command_finished_cb(AppWindow *win, const char *output,
                                gint64 elapsed, gboolean exited_cleanly);

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      AppWindow *win);
static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      AppWindow *win);
static gboolean on_close_request(GtkWindow *window, gpointer user_data);
static void on_prompt_changed(GtkTextBuffer *buffer, AppWindow *win);
static void on_dropdown_changed(GObject *self, GParamSpec *pspec, AppWindow *win);
static void update_cmd_preview(AppWindow *win);
static void app_window_restore_state(AppWindow *win);
static void on_gutter_mark(GtkSourceGutterRenderer *renderer,
                           GtkTextIter *iter,
                           GdkRectangle *area,
                           gpointer user_data);
static void apply_default_marks(GtkTextBuffer *buf);
static char *get_marked_text(AppWindow *win);
static GdkPixbuf *create_mark_pixbuf(void);

static void load_css(void);

/* ── public interface ──────────────────────────────────────────── */

AppWindow *app_window_new(GtkApplication *app)
{
    AppWindow *win;
    GtkWidget *outer_box, *row, *scroll, *label;
    GtkEventController *key_ctrl;
    GtkStringList *list;
    GtkTextBuffer *buffer;
    const char **opts;
    int opt_count, i;
    gboolean has_options;

    win = g_new0(AppWindow, 1);

    win->app = app;
    load_css();

    gtk_accelerator_parse(KB_FOCUS_PROMPT, &win->kb_focus_keyval,
                          &win->kb_focus_mods);
    gtk_accelerator_parse(KB_COPY_MARKED, &win->kb_copy_keyval,
                          &win->kb_copy_mods);
    gtk_accelerator_parse(KB_QUIT, &win->kb_quit_keyval,
                          &win->kb_quit_mods);

    win->window = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(win->window), app);
    gtk_window_set_title(GTK_WINDOW(win->window), "Promptr");
    gtk_window_set_decorated(GTK_WINDOW(win->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win->window),
                                DEFAULT_WIDTH, DEFAULT_HEIGHT);
    gtk_window_set_resizable(GTK_WINDOW(win->window), TRUE);

#if LAYER_SHELL_ENABLED
    gtk_layer_init_for_window(GTK_WINDOW(win->window));
    gtk_layer_set_layer(GTK_WINDOW(win->window),
                        GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win->window), "promptr");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
#endif

    g_signal_connect(win->window, "close-request",
                     G_CALLBACK(on_close_request), win);

    {
        GtkEventController *winctrl;

        winctrl = gtk_event_controller_key_new();
        gtk_widget_add_controller(win->window, winctrl);
        g_signal_connect(winctrl, "key-pressed",
                         G_CALLBACK(on_window_key_pressed), win);
    }

    /* ── outer vertical box ─────────────────────────────────── */
    outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(outer_box, 12);
    gtk_widget_set_margin_end(outer_box, 12);
    gtk_widget_set_margin_top(outer_box, 12);
    gtk_widget_set_margin_bottom(outer_box, 12);
    gtk_window_set_child(GTK_WINDOW(win->window), outer_box);

    /* ── row 1: prompt input ────────────────────────────────── */
    label = gtk_label_new("Prompt:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_box_append(GTK_BOX(outer_box), label);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(scroll), 80);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);

    win->prompt_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->prompt_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->prompt_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->prompt_view), 4);
    gtk_widget_add_css_class(win->prompt_view, "monospace");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  win->prompt_view);

    {
        GtkWidget *overlay, *plabel;

        overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);

        plabel = gtk_label_new(
            "E.g. list all files in current dir, except .md files");
        gtk_widget_set_halign(plabel, GTK_ALIGN_START);
        gtk_widget_set_valign(plabel, GTK_ALIGN_START);
        gtk_widget_set_margin_start(plabel, 12);
        gtk_widget_set_margin_top(plabel, 6);
        gtk_widget_set_opacity(plabel, 0.5);
        gtk_widget_add_css_class(plabel, "dim-label");
        gtk_widget_add_css_class(plabel, "monospace");
        gtk_widget_set_can_target(plabel, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), plabel);
        win->placeholder_label = plabel;

        gtk_box_append(GTK_BOX(outer_box), overlay);
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->prompt_view));
    g_signal_connect(buffer, "changed", G_CALLBACK(on_prompt_changed), win);

    key_ctrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(win->prompt_view, key_ctrl);
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_prompt_key_pressed), win);

    /* ── row 2: agent, model, submit ────────────────────────── */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(row, 4);

    label = gtk_label_new("Agent:");
    gtk_widget_set_margin_end(label, 4);
    gtk_box_append(GTK_BOX(row), label);

    opt_count = 0;
    for (opts = AGENT_OPTIONS; *opts != NULL; opts++) opt_count++;
    has_options = (opt_count > 1);
    list = gtk_string_list_new(NULL);
    for (i = 0; AGENT_OPTIONS[i] != NULL; i++)
        gtk_string_list_append(list, AGENT_OPTIONS[i]);
    win->agent_dropdown =
        gtk_drop_down_new(G_LIST_MODEL(list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(win->agent_dropdown), 0);
    gtk_widget_set_sensitive(win->agent_dropdown, has_options);
    g_signal_connect(win->agent_dropdown, "notify::selected",
                     G_CALLBACK(on_dropdown_changed), win);
    gtk_box_append(GTK_BOX(row), win->agent_dropdown);

    label = gtk_label_new("Model:");
    gtk_widget_set_margin_end(label, 4);
    gtk_box_append(GTK_BOX(row), label);

    opt_count = 0;
    for (opts = MODEL_OPTIONS; *opts != NULL; opts++) opt_count++;
    has_options = (opt_count > 1);
    list = gtk_string_list_new(NULL);
    for (i = 0; MODEL_OPTIONS[i] != NULL; i++)
        gtk_string_list_append(list, MODEL_OPTIONS[i]);
    win->model_dropdown =
        gtk_drop_down_new(G_LIST_MODEL(list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(win->model_dropdown), 0);
    gtk_widget_set_sensitive(win->model_dropdown, has_options);
    g_signal_connect(win->model_dropdown, "notify::selected",
                     G_CALLBACK(on_dropdown_changed), win);
    gtk_box_append(GTK_BOX(row), win->model_dropdown);

    win->submit_btn = gtk_button_new_with_label("Submit");
    gtk_widget_set_margin_start(win->submit_btn, 8);
    gtk_widget_add_css_class(win->submit_btn, "suggested-action");
    gtk_widget_set_sensitive(win->submit_btn, FALSE);
    g_signal_connect_swapped(win->submit_btn, "clicked",
                             G_CALLBACK(on_submit), win);
    gtk_box_append(GTK_BOX(row), win->submit_btn);

    win->cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_valign(win->cancel_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(win->cancel_btn, 8);
    gtk_widget_set_visible(win->cancel_btn, FALSE);
    g_signal_connect_swapped(win->cancel_btn, "clicked",
                             G_CALLBACK(on_cancel), win);
    gtk_box_append(GTK_BOX(row), win->cancel_btn);

    gtk_box_append(GTK_BOX(outer_box), row);

    /* ── row 3: cmd label ────────────────────────────────────── */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(row, 4);

    win->cmd_label = gtk_label_new("CMD: opencode run <query>");
    gtk_label_set_xalign(GTK_LABEL(win->cmd_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(win->cmd_label), TRUE);
    gtk_widget_set_hexpand(win->cmd_label, TRUE);
    gtk_widget_add_css_class(win->cmd_label, "monospace");
    gtk_box_append(GTK_BOX(row), win->cmd_label);

    gtk_box_append(GTK_BOX(outer_box), row);

    /* ── row 4: output ──────────────────────────────────────── */
    label = gtk_label_new("Output:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_box_append(GTK_BOX(outer_box), label);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    win->output_view = GTK_WIDGET(gtk_source_view_new());
    gtk_text_view_set_editable(GTK_TEXT_VIEW(win->output_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->output_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->output_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->output_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->output_view), 4);
    gtk_source_view_set_show_line_numbers(
        GTK_SOURCE_VIEW(win->output_view), TRUE);
    gtk_widget_add_css_class(win->output_view, "monospace");

    {
        GtkSourceGutter *gutter;
        GtkSourceGutterRenderer *marker;
        GdkPixbuf *dot;

        gutter = gtk_source_view_get_gutter(
            GTK_SOURCE_VIEW(win->output_view),
            GTK_TEXT_WINDOW_LEFT);

        dot = create_mark_pixbuf();
        marker = gtk_source_gutter_renderer_pixbuf_new();
        gtk_source_gutter_renderer_pixbuf_set_pixbuf(
            GTK_SOURCE_GUTTER_RENDERER_PIXBUF(marker), dot);
        g_object_unref(dot);
        gtk_source_gutter_renderer_set_xalign(marker, 0.5f);
        g_signal_connect(marker, "activate",
                         G_CALLBACK(on_gutter_mark), win);
        gtk_source_gutter_insert(gutter, marker, -1);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  win->output_view);
    win->output_scroll = scroll;
    gtk_box_append(GTK_BOX(outer_box), scroll);

    /* ── row 5: copy, close, quit ───────────────────────────── */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(row, 4);

    win->copy_btn = gtk_button_new_with_label("Copy");
    gtk_widget_set_sensitive(win->copy_btn, FALSE);
    g_signal_connect_swapped(win->copy_btn, "clicked",
                             G_CALLBACK(on_copy), win);
    gtk_box_append(GTK_BOX(row), win->copy_btn);

    win->close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(win->close_btn, "clicked",
                             G_CALLBACK(on_close), win);
    gtk_box_append(GTK_BOX(row), win->close_btn);

    win->quit_btn = gtk_button_new_with_label("Close & Quit");
    gtk_widget_add_css_class(win->quit_btn, "destructive-action");
    g_signal_connect_swapped(win->quit_btn, "clicked",
                             G_CALLBACK(on_quit), win);
    gtk_box_append(GTK_BOX(row), win->quit_btn);

    gtk_box_append(GTK_BOX(outer_box), row);

    app_window_restore_state(win);
    update_cmd_preview(win);

    return win;
}

void app_window_show(AppWindow *win)
{
    gtk_window_present(GTK_WINDOW(win->window));
    set_prompt_focused(win);
}

void app_window_present(AppWindow *win)
{
    gtk_window_present(GTK_WINDOW(win->window));
    set_prompt_focused(win);
}

void app_window_close_and_quit(AppWindow *win)
{
    if (win == NULL) return;
    if (win->subprocess != NULL) {
        command_cancel(win);
    }
    if (win->cancellable != NULL) {
        g_cancellable_cancel(win->cancellable);
    }
    win->destroyed = TRUE;
    gtk_window_destroy(GTK_WINDOW(win->window));
}

void app_window_free(gpointer data)
{
    AppWindow *win = data;

    if (win == NULL) return;
    if (win->subprocess != NULL) {
        command_cancel(win);
    }
    g_clear_object(&win->cancellable);
    g_free(win->cmd_string);
    g_free(win);
}

/* ── state management ──────────────────────────────────────────── */

static void set_load_state_common(AppWindow *win, gboolean loading)
{
    gtk_widget_set_sensitive(win->prompt_view, !loading);
    gtk_widget_set_sensitive(win->agent_dropdown, !loading);
    gtk_widget_set_sensitive(win->model_dropdown, !loading);
    gtk_widget_set_sensitive(win->submit_btn, !loading);
    gtk_widget_set_visible(win->cancel_btn, loading);
    gtk_widget_set_sensitive(win->cancel_btn, loading);
}

static void set_loading_state(AppWindow *win, const char *cmd)
{
    char *label_text;

    win->state = STATE_LOADING;
    set_load_state_common(win, TRUE);

    label_text = g_strdup_printf("Running: %s", cmd);
    gtk_label_set_text(GTK_LABEL(win->cmd_label), label_text);
    g_free(label_text);

    update_submit_sensitivity(win);
}

static void set_finished_state(AppWindow *win, char *cmd, gint64 elapsed,
                               const char *output)
{
    char *label_text;
    gint64 ms;

    set_load_state_common(win, FALSE);
    win->state = STATE_FINISHED;

    ms = elapsed / 1000;
    label_text = g_strdup_printf("CMD: %s  Finished. Took %" G_GINT64_FORMAT "ms.",
                                 cmd, ms);
    gtk_label_set_text(GTK_LABEL(win->cmd_label), label_text);
    g_free(label_text);
    g_free(cmd);

    if (output != NULL && output[0] != '\0') {
        GtkTextBuffer *buf;
        buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
        gtk_text_buffer_set_text(buf, output, -1);
        apply_default_marks(buf);
        gtk_widget_set_sensitive(win->copy_btn, TRUE);
    } else {
        GtkTextBuffer *buf;
        buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
        gtk_text_buffer_set_text(buf, "", -1);
        gtk_widget_set_sensitive(win->copy_btn, FALSE);
    }

    update_submit_sensitivity(win);
}

static void set_canceled_state(AppWindow *win, char *cmd)
{
    char *label_text;

    set_load_state_common(win, FALSE);
    win->state = STATE_CANCELED;

    label_text = g_strdup_printf("CMD: %s  Cancelled.", cmd);
    gtk_label_set_text(GTK_LABEL(win->cmd_label), label_text);
    g_free(label_text);
    g_free(cmd);

    update_submit_sensitivity(win);
}

/* ── submit ────────────────────────────────────────────────────── */

static void on_submit(AppWindow *win)
{
    char *query, *agent, *model;
    GString *display;
    char *cmd_display;
    GtkTextBuffer *outbuf;

    if (win->subprocess != NULL) return;

    query = get_trimmed_text(win->prompt_view);
    if (query == NULL || query[0] == '\0') {
        g_free(query);
        return;
    }

    agent = get_selected_text(win->agent_dropdown);
    model = get_selected_text(win->model_dropdown);

    display = g_string_new("opencode run");
    if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
        g_string_append_printf(display, " --model %s", model);
    if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
        g_string_append_printf(display, " --agent %s", agent);
    g_string_append_printf(display, " %s", query);
    cmd_display = g_string_free(display, FALSE);

    g_free(win->cmd_string);
    win->cmd_string = cmd_display;

    outbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
    gtk_text_buffer_set_text(outbuf, "", -1);
    gtk_widget_set_sensitive(win->copy_btn, FALSE);

    command_execute(win, model, agent, query, command_finished_cb);

    if (win->subprocess != NULL)
        set_loading_state(win, cmd_display);

    g_free(query);
    g_free(agent);
    g_free(model);
}

static void command_finished_cb(AppWindow *win, const char *output,
                                gint64 elapsed, gboolean exited_cleanly)
{
    char *cmd;

    cmd = win->cmd_string;
    win->cmd_string = NULL;

    if (win->destroyed) {
        g_free(cmd);
        return;
    }

    if (exited_cleanly) {
        set_finished_state(win, cmd, elapsed, output);
    } else {
        set_canceled_state(win, cmd);
    }
}

/* ── cancel ────────────────────────────────────────────────────── */

static void on_cancel(AppWindow *win)
{
    if (win->subprocess == NULL) return;
    command_cancel(win);
}

/* ── copy ──────────────────────────────────────────────────────── */

static void on_copy(AppWindow *win)
{
    char *text;
    GdkClipboard *clipboard;

    text = get_marked_text(win);
    clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(win->window));
    gdk_clipboard_set_text(clipboard, text);
    g_free(text);
}

/* ── close / quit ──────────────────────────────────────────────── */

static void on_close(AppWindow *win)
{
    if (win->subprocess != NULL)
        command_cancel(win);
    gtk_widget_set_visible(win->window, FALSE);
}

static void on_quit(AppWindow *win)
{
    g_application_quit(G_APPLICATION(win->app));
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data)
{
    AppWindow *win = user_data;

    (void)window;
    if (win->subprocess != NULL)
        command_cancel(win);
    gtk_widget_set_visible(win->window, FALSE);
    return TRUE;
}

/* ── key handling ──────────────────────────────────────────────── */

static gboolean on_prompt_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      AppWindow *win)
{
    (void)controller;
    (void)keycode;

    if (keyval == GDK_KEY_Escape) {
#if ESCAPE_HIDES_WINDOW
        gtk_widget_set_visible(win->window, FALSE);
#endif
        return GDK_EVENT_STOP;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if ((state & GDK_SHIFT_MASK) == 0) {
            if (gtk_widget_is_sensitive(win->submit_btn)
                && win->subprocess == NULL) {
                on_submit(win);
            }
            return GDK_EVENT_STOP;
        }
    }

    return GDK_EVENT_PROPAGATE;
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      AppWindow *win)
{
    GtkTextBuffer *buf;
    GtkTextIter start, end;
    GdkModifierType mods;

    (void)controller;
    (void)keycode;

    mods = state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK
                    | GDK_ALT_MASK | GDK_SUPER_MASK);

    if (keyval == win->kb_quit_keyval && mods == win->kb_quit_mods) {
        on_quit(win);
        return GDK_EVENT_STOP;
    }

    if (keyval == win->kb_focus_keyval && mods == win->kb_focus_mods) {
        gtk_widget_grab_focus(win->prompt_view);
        buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->prompt_view));
        gtk_text_buffer_get_bounds(buf, &start, &end);
        gtk_text_buffer_select_range(buf, &start, &end);
        return GDK_EVENT_STOP;
    }

    if (keyval == win->kb_copy_keyval && mods == win->kb_copy_mods) {
        if (gtk_widget_is_sensitive(win->copy_btn))
            on_copy(win);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/* ── prompt change ─────────────────────────────────────────────── */

static void on_prompt_changed(GtkTextBuffer *buffer, AppWindow *win)
{
    (void)buffer;
    gtk_widget_set_visible(win->placeholder_label,
        gtk_text_buffer_get_char_count(buffer) == 0);
    update_submit_sensitivity(win);
    update_cmd_preview(win);
}

static void update_submit_sensitivity(AppWindow *win)
{
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

static char *get_trimmed_text(GtkWidget *text_view)
{
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

static char *get_selected_text(GtkWidget *dropdown)
{
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

static void set_prompt_focused(AppWindow *win)
{
    gtk_widget_grab_focus(win->prompt_view);
}

/* ── dropdown change ──────────────────────────────────────────── */

static void on_dropdown_changed(GObject *self, GParamSpec *pspec,
                                AppWindow *win)
{
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

static void update_cmd_preview(AppWindow *win)
{
    char *query, *agent, *model;
    GString *display;

    if (win->state == STATE_LOADING) return;

    if (win->state == STATE_FINISHED || win->state == STATE_CANCELED)
        win->state = STATE_IDLE;

    query = get_trimmed_text(win->prompt_view);
    agent = get_selected_text(win->agent_dropdown);
    model = get_selected_text(win->model_dropdown);

    display = g_string_new("CMD: opencode run");
    if (model != NULL
        && g_strcmp0(model, "None") != 0
        && model[0] != '\0')
        g_string_append_printf(display, " --model %s", model);
    if (agent != NULL
        && g_strcmp0(agent, "None") != 0
        && agent[0] != '\0')
        g_string_append_printf(display, " --agent %s", agent);
    if (query != NULL && query[0] != '\0')
        g_string_append_printf(display, " %s", query);
    else
        g_string_append(display, " <query>");

    gtk_label_set_text(GTK_LABEL(win->cmd_label), display->str);
    g_string_free(display, TRUE);
    g_free(query);
    g_free(agent);
    g_free(model);
}

/* ── gutter marks ──────────────────────────────────────────────── */

static GdkPixbuf *create_mark_pixbuf(void)
{
    GdkPixbuf *p;

    p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 10, 10);
    gdk_pixbuf_fill(p, 0x3584e4ff);
    return p;
}

static void on_gutter_mark(GtkSourceGutterRenderer *renderer,
                           GtkTextIter *iter,
                           GdkRectangle *area,
                           gpointer user_data)
{
    GtkTextBuffer *buf;
    int line;
    GSList *marks;

    (void)renderer;
    (void)area;
    (void)user_data;

    buf = gtk_text_iter_get_buffer(iter);
    line = gtk_text_iter_get_line(iter);

    marks = gtk_source_buffer_get_source_marks_at_line(
        GTK_SOURCE_BUFFER(buf), line, "promptr-mark");

    if (marks != NULL) {
        for (GSList *m = marks; m != NULL; m = m->next)
            gtk_text_buffer_delete_mark(buf, GTK_TEXT_MARK(m->data));
    } else {
        gtk_source_buffer_create_source_mark(
            GTK_SOURCE_BUFFER(buf), NULL, "promptr-mark", iter);
    }

    if (marks != NULL)
        g_slist_free(marks);
}

static void apply_default_marks(GtkTextBuffer *buf)
{
    GtkTextIter iter;
    int i, total, line;
    gboolean mark_all;

    if (DEFAULT_MARKED_LINES[0] == -1) return;

    mark_all = (DEFAULT_MARKED_LINES[0] == 0);

    if (mark_all) {
        gtk_text_buffer_get_start_iter(buf, &iter);
        while (!gtk_text_iter_is_end(&iter)) {
            gtk_source_buffer_create_source_mark(
                GTK_SOURCE_BUFFER(buf), NULL, "promptr-mark", &iter);
            gtk_text_iter_forward_line(&iter);
        }
        return;
    }

    total = gtk_text_buffer_get_line_count(buf);
    for (i = 0; DEFAULT_MARKED_LINES[i] != -1; i++) {
        line = DEFAULT_MARKED_LINES[i] - 1;
        if (line >= 0 && line < total) {
            gtk_text_buffer_get_iter_at_line(buf, &iter, line);
            gtk_source_buffer_create_source_mark(
                GTK_SOURCE_BUFFER(buf), NULL, "promptr-mark", &iter);
        }
    }
}

static char *get_marked_text(AppWindow *win)
{
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

        marks = gtk_source_buffer_get_source_marks_at_line(
            GTK_SOURCE_BUFFER(buf), line, "promptr-mark");
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

void app_window_save_state(AppWindow *win)
{
    char *agent, *model;

    agent = get_selected_text(win->agent_dropdown);
    model = get_selected_text(win->model_dropdown);
    state_save(model, agent);
    g_free(agent);
    g_free(model);
}

static void select_option_by_value(GtkWidget *dropdown, const char *value)
{
    GListModel *model;
    guint i, n;

    if (value == NULL) return;

    model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
    n = g_list_model_get_n_items(model);
    for (i = 0; i < n; i++) {
        g_autoptr(GObject) item = g_list_model_get_item(model, i);
        const char *str;

        if (item == NULL) continue;
        str = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
        if (str != NULL && g_strcmp0(str, value) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
            return;
        }
    }
}

static void app_window_restore_state(AppWindow *win)
{
    g_autofree char *model = NULL;
    g_autofree char *agent = NULL;

    if (!state_load(&model, &agent)) return;

    if (model != NULL)
        select_option_by_value(win->model_dropdown, model);
    if (agent != NULL)
        select_option_by_value(win->agent_dropdown, agent);
}

/* ── CSS ───────────────────────────────────────────────────────── */

static void load_css(void)
{
    static gboolean loaded = FALSE;
    GtkCssProvider *provider;
    GdkDisplay *display;

    if (loaded) return;
    loaded = TRUE;

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "textview.monospace, label.monospace { font-family: monospace; }"
        "textview gutter {"
        "  background-color: @theme_bg_color;"
        "  color: @theme_fg_color;"
        "}"
        "dropdown button arrow {"
        "  -gtk-icon-source: -gtk-icontheme(\"pan-down-symbolic\");"
        "}");
    display = gdk_display_get_default();
    gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}
