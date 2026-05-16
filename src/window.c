#include "window.h"
#include "command.h"
#include "state.h"
#include "config.h"
#include "configfile.h"

#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <gtksourceview/gtksource.h>
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
                                const char *stderr_output,
                                gint64 elapsed, int exit_code,
                                gboolean exited_cleanly);

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
static void set_cmd_text(AppWindow *win, const char *text);
static gboolean hex_to_rgba(const char *hex, GdkRGBA *out);
static void on_gutter_click(GtkGestureClick *gesture,
                            int n_press,
                            double x, double y,
                            AppWindow *win);
static void update_marked_label(AppWindow *win);
static void apply_default_marks(AppWindow *win, GtkTextBuffer *buf);
static char *get_marked_text(AppWindow *win);

static void load_css(void);

/* ── public interface ──────────────────────────────────────────── */

AppWindow *app_window_new(GtkApplication *app)
{
    AppWindow *win;
    GtkWidget *outer_box, *row, *scroll, *label;
    GtkEventController *key_ctrl;
    GtkStringList *list;
    GtkTextBuffer *buffer;
    int i;
    gboolean has_options;
    g_autofree char *kb;
    g_autofree char **opts = NULL;

    win = g_new0(AppWindow, 1);

    win->app = app;
    win->config = runtime_config_load();
    win->marked_lines_str = runtime_config_get_string(
        win->config, "marked_lines", DEFAULT_MARKED_LINES_STR);

    {
        g_autofree char *raw;

        raw = runtime_config_get_string(win->config, "opencode_path",
                                        OPENCODE_PATH);
        if (g_str_has_prefix(raw, "~/"))
            win->opencode_bin = g_build_filename(g_get_home_dir(),
                                                 raw + 2, NULL);
        else
            win->opencode_bin = g_strdup(raw);
    }
    load_css();

    kb = runtime_config_get_string(win->config, "kb_focus_prompt",
                                   KB_FOCUS_PROMPT);
    gtk_accelerator_parse(kb, &win->kb_focus_keyval,
                          &win->kb_focus_mods);
    kb = runtime_config_get_string(win->config, "kb_copy_marked",
                                   KB_COPY_MARKED);
    gtk_accelerator_parse(kb, &win->kb_copy_keyval,
                          &win->kb_copy_mods);
    kb = runtime_config_get_string(win->config, "kb_quit",
                                   KB_QUIT);
    gtk_accelerator_parse(kb, &win->kb_quit_keyval,
                          &win->kb_quit_mods);

    win->window = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(win->window), app);
    gtk_window_set_title(GTK_WINDOW(win->window), "Promptr");
    gtk_window_set_decorated(GTK_WINDOW(win->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win->window),
        runtime_config_get_int(win->config, "width", DEFAULT_WIDTH),
        runtime_config_get_int(win->config, "height", DEFAULT_HEIGHT));
    gtk_window_set_resizable(GTK_WINDOW(win->window), TRUE);

    if (runtime_config_get_bool(win->config, "layer_shell",
                                LAYER_SHELL_ENABLED)) {
        gtk_layer_init_for_window(GTK_WINDOW(win->window));
        gtk_layer_set_layer(GTK_WINDOW(win->window),
                            GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win->window), "promptr");
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win->window),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    }

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
    label = gtk_label_new("Prompt");
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
    win->agent_dropdown =
        gtk_drop_down_new(G_LIST_MODEL(list), NULL);
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
    win->model_dropdown =
        gtk_drop_down_new(G_LIST_MODEL(list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(win->model_dropdown), 0);
    gtk_widget_set_sensitive(win->model_dropdown, has_options);
    g_signal_connect(win->model_dropdown, "notify::selected",
                     G_CALLBACK(on_dropdown_changed), win);
    gtk_box_append(GTK_BOX(row), win->model_dropdown);
    g_strfreev(opts);
    opts = NULL;

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

    /* ── row 3: cmd label + preview ───────────────────────────── */
    label = gtk_label_new("Command");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_box_append(GTK_BOX(outer_box), label);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(scroll), 36);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);

    win->cmd_label = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(win->cmd_label), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->cmd_label), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->cmd_label),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->cmd_label), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->cmd_label), 4);
    gtk_widget_set_hexpand(win->cmd_label, TRUE);
    gtk_widget_add_css_class(win->cmd_label, "monospace");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  win->cmd_label);
    gtk_box_append(GTK_BOX(outer_box), scroll);

    /* ── row 4: output ──────────────────────────────────────── */
    label = gtk_label_new("Output");
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
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->output_view), 2);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(win->output_view), 4);
    gtk_source_view_set_show_line_numbers(
        GTK_SOURCE_VIEW(win->output_view), TRUE);
    gtk_widget_add_css_class(win->output_view, "monospace");

    gtk_source_view_set_show_line_marks(
        GTK_SOURCE_VIEW(win->output_view), TRUE);

    {
        GdkRGBA c;
        g_autofree char *color;

        color = runtime_config_get_string(win->config, "mark_bg_color",
                                          MARK_BG_COLOR);
        hex_to_rgba(color, &c);
        GtkSourceMarkAttributes *attrs;

        attrs = gtk_source_mark_attributes_new();
        gtk_source_mark_attributes_set_background(attrs, &c);
        gtk_source_mark_attributes_set_icon_name(attrs,
            "media-record-symbolic");
        gtk_source_view_set_mark_attributes(
            GTK_SOURCE_VIEW(win->output_view),
            "promptr-mark", attrs, 0);
    }

    {
        GtkGesture *gesture;

        gesture = gtk_gesture_click_new();
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                      GDK_BUTTON_PRIMARY);
        gtk_widget_add_controller(win->output_view,
                                  GTK_EVENT_CONTROLLER(gesture));
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(on_gutter_click), win);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  win->output_view);
    win->output_scroll = scroll;
    gtk_box_append(GTK_BOX(outer_box), scroll);

    /* ── row 5: marked lines label ──────────────────────────── */
    win->marked_label = gtk_label_new("Marked: none");
    gtk_label_set_xalign(GTK_LABEL(win->marked_label), 0.0f);
    gtk_widget_set_margin_top(win->marked_label, 4);
    gtk_widget_set_margin_bottom(win->marked_label, 4);
    gtk_box_append(GTK_BOX(outer_box), win->marked_label);

    /* ── row 6: copy, close, quit ───────────────────────────── */
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

static void remove_temp_dirs(AppWindow *win)
{
    GSList *l;
    GDir *dir;
    const char *name;
    g_autofree char *path = NULL;

    if (win->temp_dirs == NULL) return;

    for (l = win->temp_dirs; l != NULL; l = l->next) {
        dir = g_dir_open((const char *)l->data, 0, NULL);
        if (dir != NULL) {
            while ((name = g_dir_read_name(dir)) != NULL) {
                path = g_build_filename((const char *)l->data,
                                        name, NULL);
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

void app_window_close_and_quit(AppWindow *win)
{
    if (win == NULL) return;
    remove_temp_dirs(win);
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
    remove_temp_dirs(win);
    if (win->subprocess != NULL) {
        command_cancel(win);
    }
    g_clear_object(&win->cancellable);
    g_free(win->cmd_string);
    g_free(win->marked_lines_str);
    g_free(win->opencode_bin);
    runtime_config_free(win->config);
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
    set_cmd_text(win, label_text);
    g_free(label_text);

    gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Running...");

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
    label_text = g_strdup_printf("%s  Finished. Took %" G_GINT64_FORMAT "ms.",
                                 cmd, ms);
    set_cmd_text(win, label_text);
    g_free(label_text);
    g_free(cmd);

    if (output != NULL && output[0] != '\0') {
        GtkTextBuffer *buf;
        buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
        gtk_text_buffer_set_text(buf, output, -1);
        if (!win->defaults_applied) {
            apply_default_marks(win, buf);
            win->defaults_applied = TRUE;
        }
        update_marked_label(win);
        gtk_widget_set_sensitive(win->copy_btn, TRUE);
    } else {
        GtkTextBuffer *buf;
        buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
        gtk_text_buffer_set_text(buf, "", -1);
        gtk_widget_set_sensitive(win->copy_btn, FALSE);
    }

    gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Submit");
    update_submit_sensitivity(win);
}

static void set_canceled_state(AppWindow *win, char *cmd)
{
    char *label_text;

    set_load_state_common(win, FALSE);
    win->state = STATE_CANCELED;

    label_text = g_strdup_printf("%s  Cancelled.", cmd);
    set_cmd_text(win, label_text);
    g_free(label_text);
    g_free(cmd);

    gtk_button_set_label(GTK_BUTTON(win->submit_btn), "Submit");
    update_submit_sensitivity(win);
}

static void set_errored_state(AppWindow *win, char *cmd,
                              const char *stderr_output)
{
    char *label_text;
    GtkTextBuffer *buf;

    set_load_state_common(win, FALSE);
    win->state = STATE_FINISHED;

    label_text = g_strdup_printf("%s  Errored.", cmd);
    set_cmd_text(win, label_text);
    g_free(label_text);
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
}

/* ── submit ────────────────────────────────────────────────────── */

static void on_submit(AppWindow *win)
{
    char *query, *agent, *model;
    GString *display;
    char *cmd_display;
    GtkTextBuffer *outbuf;
    g_autofree char *tmpdir = NULL;
    GError *err = NULL;

    if (win->subprocess != NULL) return;

    query = get_trimmed_text(win->prompt_view);
    if (query == NULL || query[0] == '\0') {
        g_free(query);
        return;
    }

    agent = get_selected_text(win->agent_dropdown);
    model = get_selected_text(win->model_dropdown);

    {
        tmpdir = g_dir_make_tmp("promptr-XXXXXX", &err);
        if (tmpdir == NULL) {
            g_warning("Failed to create temp dir: %s", err->message);
            g_clear_error(&err);
        }
    }

    display = g_string_new(win->opencode_bin);
    g_string_append(display, " run");
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
    gtk_text_buffer_set_text(outbuf, "", -1);
    gtk_widget_set_sensitive(win->copy_btn, FALSE);

    command_execute(win, model, agent, query, tmpdir,
                    win->opencode_bin,
                    command_finished_cb);

    if (win->subprocess != NULL && tmpdir != NULL)
        win->temp_dirs = g_slist_prepend(win->temp_dirs,
                                         g_strdup(tmpdir));

    if (win->subprocess != NULL)
        set_loading_state(win, cmd_display);

    g_free(query);
    g_free(agent);
    g_free(model);
}

static void command_finished_cb(AppWindow *win, const char *output,
                                const char *stderr_output,
                                gint64 elapsed, int exit_code,
                                gboolean exited_cleanly)
{
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

static void on_cancel(AppWindow *win)
{
    if (win->subprocess == NULL) return;
    command_cancel(win);
}

/* ── copy ──────────────────────────────────────────────────────── */

static gboolean flash_restore(gpointer user_data)
{
    AppWindow *win = user_data;

    update_marked_label(win);
    return G_SOURCE_REMOVE;
}

static void on_copy(AppWindow *win)
{
    char *text;
    GdkClipboard *clipboard;

    text = get_marked_text(win);
    clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(win->window));
    gdk_clipboard_set_text(clipboard, text);

    if (runtime_config_get_bool(win->config, "notify_on_copy",
                                NOTIFY_ON_COPY)) {
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

    gtk_label_set_text(GTK_LABEL(win->marked_label), "Copied!");
    g_timeout_add(1500, flash_restore, win);
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
        if (runtime_config_get_bool(win->config, "escape_hides",
                                    ESCAPE_HIDES_WINDOW))
            gtk_widget_set_visible(win->window, FALSE);
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

    keyval = gdk_keyval_to_lower(keyval);
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

static void set_cmd_text(AppWindow *win, const char *text)
{
    GtkTextBuffer *buf;

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->cmd_label));
    gtk_text_buffer_set_text(buf, text != NULL ? text : "", -1);
}

static gboolean hex_to_rgba(const char *hex, GdkRGBA *out)
{
    unsigned int r, g, b;
    double a;

    if (hex == NULL || hex[0] != '#') return FALSE;

    if (sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
        a = 0.60;
    } else if (sscanf(hex + 1, "%2x%2x%2x%2x", &r, &g, &b,
                      (unsigned int *)&a) == 4) {
        a /= 255.0;
    } else {
        return FALSE;
    }

    out->red   = r / 255.0;
    out->green = g / 255.0;
    out->blue  = b / 255.0;
    out->alpha = a;
    return TRUE;
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

    display = g_string_new(win->opencode_bin);
    g_string_append(display, " run --dir <tmp>");
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

    set_cmd_text(win, display->str);
    g_string_free(display, TRUE);
    g_free(query);
    g_free(agent);
    g_free(model);
}

/* ── gutter marks ──────────────────────────────────────────────── */

static void toggle_mark_at_iter(GtkTextBuffer *buf, GtkTextIter *iter)
{
    int line;
    GSList *marks;

    line = gtk_text_iter_get_line(iter);
    marks = gtk_source_buffer_get_source_marks_at_line(
        GTK_SOURCE_BUFFER(buf), line, "promptr-mark");

    if (marks != NULL) {
        for (GSList *m = marks; m != NULL; m = m->next)
            gtk_text_buffer_delete_mark(buf, GTK_TEXT_MARK(m->data));
        g_slist_free(marks);
    } else {
        gtk_source_buffer_create_source_mark(
            GTK_SOURCE_BUFFER(buf), NULL, "promptr-mark", iter);
    }
}

static void on_gutter_click(GtkGestureClick *gesture,
                            int n_press,
                            double x, double y,
                            AppWindow *win)
{
    GtkTextBuffer *buf;
    GtkTextIter iter;
    int buf_x, buf_y;

    (void)gesture;
    (void)n_press;

    gtk_text_view_window_to_buffer_coords(
        GTK_TEXT_VIEW(win->output_view),
        GTK_TEXT_WINDOW_WIDGET,
        (int)x, (int)y, &buf_x, &buf_y);

    if (buf_x >= 0) return;

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->output_view));
    gtk_text_view_get_iter_at_location(
        GTK_TEXT_VIEW(win->output_view), &iter, buf_x, buf_y);
    toggle_mark_at_iter(buf, &iter);
    update_marked_label(win);
}

static void update_marked_label(AppWindow *win)
{
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
        marks = gtk_source_buffer_get_source_marks_at_line(
            GTK_SOURCE_BUFFER(buf), line, "promptr-mark");
        if (marks != NULL) {
            marked++;
            g_slist_free(marks);
        }
        gtk_text_iter_forward_line(&iter);
        line++;
    }

    if (total <= 0) {
        gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked: none");
        return;
    }

    if (marked == 0) {
        gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked: none");
        return;
    }

    if (marked == total) {
        gtk_label_set_text(GTK_LABEL(win->marked_label), "Marked: all");
        return;
    }

    label = g_string_new("Marked: ");
    line = 0;
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (!gtk_text_iter_is_end(&iter)) {
        GSList *marks;
        marks = gtk_source_buffer_get_source_marks_at_line(
            GTK_SOURCE_BUFFER(buf), line, "promptr-mark");
        if (marks != NULL) {
            if (label->len > 8)
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

static void apply_default_marks(AppWindow *win, GtkTextBuffer *buf)
{
    GtkTextIter iter;
    int i, total, line;
    gboolean mark_all;
    g_autofree char **parts = NULL;

    if (g_strcmp0(win->marked_lines_str, "-1") == 0) return;

    mark_all = (g_strcmp0(win->marked_lines_str, "0") == 0);

    if (mark_all) {
        gtk_text_buffer_get_start_iter(buf, &iter);
        while (!gtk_text_iter_is_end(&iter)) {
            gtk_source_buffer_create_source_mark(
                GTK_SOURCE_BUFFER(buf), NULL, "promptr-mark", &iter);
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
