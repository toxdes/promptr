#ifndef COMMAND_H
#define COMMAND_H

#include <gtk/gtk.h>
#include "window.h"

typedef void (*CommandCallback)(AppWindow *win,
                                const char *output,
                                const char *stderr_output,
                                gint64      elapsed_us,
                                int         exit_code,
                                gboolean    exited_cleanly);

void command_execute(AppWindow      *win,
                     const char     *model,
                     const char     *agent,
                     const char     *query,
                     const char     *workdir,
                     const char     *opencode_bin,
                     CommandCallback callback);

void command_cancel(AppWindow *win);

#endif
