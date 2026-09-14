#ifndef COMMAND_H
#define COMMAND_H

#include "tab.h"
#include <gtk/gtk.h>

typedef void (*CommandCallback)(Tab *tab, const char *output,
                                const char *stderr_output, gint64 elapsed_us,
                                int exit_code, gboolean exited_cleanly);

void command_execute_argv(Tab *tab, char **argv, const char *cwd,
                          CommandCallback callback);

void command_cancel(Tab *tab);

#endif
