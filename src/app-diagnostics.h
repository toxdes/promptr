#ifndef APP_DIAGNOSTICS_H
#define APP_DIAGNOSTICS_H

#include <glib.h>

/* Downgrade only verified GTK 4.24.0 through 4.24.2 diagnostics. */
gboolean app_diagnostics_is_gtk_noise(GLogLevelFlags level,
                                      const GLogField *fields, gsize n_fields,
                                      guint major, guint minor, guint micro);
void app_diagnostics_install(guint major, guint minor, guint micro);

#endif
