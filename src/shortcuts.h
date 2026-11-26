#ifndef SHORTCUTS_H
#define SHORTCUTS_H

#include "configfile.h"

typedef struct {
  const char *group, *label, *key, *fallback;
} ShortcutDefinition;

const ShortcutDefinition *shortcut_definitions(gsize *count);
char *shortcut_value(RuntimeConfig *config, const char *key);
char *shortcut_human(RuntimeConfig *config, const char *key);
char *accel_to_human(const char *accel);

#endif
