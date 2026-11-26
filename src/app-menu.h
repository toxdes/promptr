#ifndef APP_MENU_H
#define APP_MENU_H
#include "configfile.h"
#include <gio/gio.h>

/* Section pointers are borrowed from the returned model. */
GMenuModel *app_menu_create(RuntimeConfig *config, GMenu **providers,
                            GMenu **agents, GMenu **models);
#endif
