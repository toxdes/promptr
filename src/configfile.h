#ifndef CONFIGFILE_H
#define CONFIGFILE_H

#include <glib.h>

typedef struct _RuntimeConfig RuntimeConfig;

RuntimeConfig *runtime_config_load(void);
void runtime_config_free(RuntimeConfig *c);

int runtime_config_get_int(RuntimeConfig *c, const char *key, int fallback);
gboolean runtime_config_get_bool(RuntimeConfig *c, const char *key,
                                 gboolean fallback);
char *runtime_config_get_string(RuntimeConfig *c, const char *key,
                                const char *fallback);
char **runtime_config_get_string_list(RuntimeConfig *c, const char *key);

#endif
