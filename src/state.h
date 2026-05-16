#ifndef STATE_H
#define STATE_H

#include <glib.h>

gboolean state_save(const char *model, const char *agent);
gboolean state_load(char **model_out, char **agent_out);

#endif
