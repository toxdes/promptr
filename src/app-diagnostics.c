#include "app-diagnostics.h"

#include <string.h>

typedef struct {
  guint major, minor, micro;
} ToolkitVersion;

static gboolean field_equals(const GLogField *field, const char *value) {
  gsize length = strlen(value);
  if (field->value == NULL)
    return FALSE;
  if (field->length < 0)
    return strcmp(field->value, value) == 0;
  return (gsize)field->length == length &&
         memcmp(field->value, value, length) == 0;
}

static gboolean consume_timing_digits(const char *text, gsize length,
                                      gsize *position, guint max_digits) {
  if (*position < length && text[*position] == '-')
    (*position)++;
  gsize start = *position;
  while (*position < length && g_ascii_isdigit(text[*position]))
    (*position)++;
  return *position > start && *position - start <= max_digits;
}

static gboolean is_duplicate_presentation(const GLogField *field) {
  static const char prefix[] =
      "Duplicate call to gdk_frame_timings_presented() with different values: "
      "presentation time is ";
  static const char suffix[] = "ms off from expected";
  if (field->value == NULL)
    return FALSE;
  const char *text = field->value;
  gsize length = field->length < 0 ? strlen(text) : (gsize)field->length;
  gsize position = sizeof(prefix) - 1;
  if (length < position || memcmp(text, prefix, position) != 0)
    return FALSE;
  /* GTK prints a signed int64 nanosecond delta as milliseconds and a
   * signed remainder; negative deltas can therefore contain two minus signs. */
  if (!consume_timing_digits(text, length, &position, 13) ||
      position >= length || text[position++] != '.' ||
      !consume_timing_digits(text, length, &position, 3))
    return FALSE;
  return length - position == sizeof(suffix) - 1 &&
         memcmp(text + position, suffix, sizeof(suffix) - 1) == 0;
}

gboolean app_diagnostics_is_gtk_noise(GLogLevelFlags level,
                                      const GLogField *fields, gsize n_fields,
                                      guint major, guint minor, guint micro) {
  static const char *const messages[] = {
      "Cannot get portal org.freedesktop.portal.Inhibit version: "
      "GDBus.Error:org.freedesktop.DBus.Error.InvalidArgs: "
      "No such interface “org.freedesktop.portal.Inhibit”",
      "gdk_frame_timings_presented() called on discarded frame.",
      "gdk_frame_timings_discarded() called on already presented frame.",
  };
  const GLogField *domain = NULL, *message = NULL;

  if (major != 4 || minor != 24 || micro > 2 ||
      (level & G_LOG_LEVEL_MASK) != G_LOG_LEVEL_WARNING ||
      (level & (G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION)) != 0 ||
      fields == NULL)
    return FALSE;
  for (gsize i = 0; i < n_fields; i++) {
    if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
      domain = &fields[i];
    else if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
      message = &fields[i];
  }
  if (domain == NULL || message == NULL || !field_equals(domain, "Gdk"))
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(messages); i++)
    if (field_equals(message, messages[i]))
      return TRUE;
  return is_duplicate_presentation(message);
}

static GLogWriterOutput write_log(GLogLevelFlags level, const GLogField *fields,
                                  gsize n_fields, gpointer data) {
  const ToolkitVersion *version = data;
  static const GDebugKey debug_keys[] = {{"fatal-warnings", 1}};
  gboolean fatal_warnings =
      g_parse_debug_string(g_getenv("G_DEBUG"), debug_keys,
                           G_N_ELEMENTS(debug_keys)) != 0;
  if (fatal_warnings ||
      !app_diagnostics_is_gtk_noise(level, fields, n_fields, version->major,
                                    version->minor, version->micro) ||
      n_fields > G_MAXSIZE / sizeof(GLogField))
    return g_log_writer_default(level, fields, n_fields, NULL);

  /* Keep original messages available through G_MESSAGES_DEBUG=Gdk.
   * Preserve severity for every other diagnostic, including fatal warnings. */
  g_autofree GLogField *debug_fields =
      g_memdup2(fields, n_fields * sizeof(GLogField));
  for (gsize i = 0; i < n_fields; i++) {
    if (g_strcmp0(debug_fields[i].key, "PRIORITY") == 0) {
      debug_fields[i].value = "7";
      debug_fields[i].length = -1;
    }
  }
  return g_log_writer_default(G_LOG_LEVEL_DEBUG, debug_fields, n_fields, NULL);
}

void app_diagnostics_install(guint major, guint minor, guint micro) {
  ToolkitVersion *version = g_new0(ToolkitVersion, 1);
  version->major = major;
  version->minor = minor;
  version->micro = micro;
  g_log_set_writer_func(write_log, version, g_free);
}
