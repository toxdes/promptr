#ifndef COMMAND_H
#define COMMAND_H

#include <gtk/gtk.h>
#include "window.h"

typedef void (*CommandCallback)(AppWindow *win,
                                const char *output,
                                gint64      elapsed_us,
                                gboolean    exited_cleanly);

void command_execute(AppWindow      *win,
                     const char     *model,
                     const char     *agent,
                     const char     *query,
                     const char     *workdir,
                     CommandCallback callback);

void command_cancel(AppWindow *win);

#endif
