#ifndef AGENT_CONTEXT_H
#define AGENT_CONTEXT_H
#include <gio/gio.h>
char **agent_context_list(void);
char *agent_context_resolve(const char *agent, GError **error);
#endif
