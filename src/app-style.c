#include "app-style.h"
#include "config.h"
#include <gtk/gtk.h>

static void append_editor_font(GString *css, const char *class_name,
                               const char *value) {
  g_autoptr(PangoFontDescription) font =
      pango_font_description_from_string(value);
  const char *family = pango_font_description_get_family(font);
  if (family == NULL || !*family)
    family = "Monospace";
  g_autoptr(GString) escaped = g_string_new(NULL);
  for (const unsigned char *ch = (const unsigned char *)family; *ch; ch++) {
    if (*ch == '\\' || *ch == '"')
      g_string_append_c(escaped, '\\');
    g_string_append_c(escaped, *ch < 0x20 || *ch == 0x7f ? ' ' : *ch);
  }
  PangoStyle style = pango_font_description_get_style(font);
  g_string_append_printf(
      css,
      "textview.%s, textview.%s text, label.%s { font-family: \"%s\"; "
      "font-weight: %d; font-style: %s;",
      class_name, class_name, class_name, escaped->str,
      pango_font_description_get_weight(font),
      style == PANGO_STYLE_ITALIC    ? "italic"
      : style == PANGO_STYLE_OBLIQUE ? "oblique"
                                     : "normal");
  int size = pango_font_description_get_size(font);
  if (size > 0) {
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(number, sizeof(number), (double)size / PANGO_SCALE);
    g_string_append_printf(
        css, " font-size: %s%s;", number,
        pango_font_description_get_size_is_absolute(font) ? "px" : "pt");
  }
  g_string_append_c(css, '}');
}

void app_style_apply(RuntimeConfig *config) {
  static GtkCssProvider *provider = NULL;
  GdkDisplay *display;
  GString *css;

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

  g_string_append(css, "textview.prompt-font { caret-color: #00ab41; }");

  g_string_append(css, ".tab-drop-target {"
                       "  border-left: 3px solid #3584e4;"
                       "}"
                       ".tab-status-dot {"
                       "  font-size: 0.6em;"
                       "  color: alpha(currentColor, 0.2);"
                       "}"
                       ".tab-status-dot.loading {"
                       "  color: #e5a50a;"
                       "}"
                       ".tab-status-dot.finished {"
                       "  color: #33cc7f;"
                       "}"
                       ".tab-status-dot.finished-empty {"
                       "  color: #c42b1c;"
                       "}");

  g_autofree char *query =
      runtime_config_get_string(config, "query_font", QUERY_FONT_DEFAULT);
  g_autofree char *answer =
      runtime_config_get_string(config, "answer_font", ANSWER_FONT_DEFAULT);
  append_editor_font(css, "prompt-font", query);
  append_editor_font(css, "output-font", answer);
  gboolean first_load = provider == NULL;
  if (first_load)
    provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css->str);
  g_string_free(css, TRUE);
  display = gdk_display_get_default();
  if (first_load)
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
