#ifndef PROVIDER_H
#define PROVIDER_H

#include <gio/gio.h>
#include <glib.h>

typedef struct _RuntimeConfig RuntimeConfig;

typedef enum {
  PROVIDER_ROLE_USER,
  PROVIDER_ROLE_ASSISTANT,
  PROVIDER_ROLE_SYSTEM,
} ProviderRole;

typedef struct {
  ProviderRole role;
  char *content;
} ProviderMessage;

typedef enum {
  PROVIDER_EVENT_CHUNK,
  PROVIDER_EVENT_DONE,
  PROVIDER_EVENT_ERROR,
} ProviderEventType;

typedef struct {
  ProviderEventType type;
  char *output;
  char *error_msg;
  gint64 elapsed_us;
} ProviderEvent;

typedef void (*ProviderCallback)(gpointer source, const ProviderEvent *event,
                                 gpointer user_data);

typedef struct _ProviderVTable {
  const char *name;

  gboolean (*init)(void **impl, RuntimeConfig *config, GError **error);
  void (*destroy)(void *impl);
  void (*cleanup_session)(void *impl, gpointer tab_key);

  gboolean (*submit)(void *impl, gpointer tab_key, const char *query,
                     const ProviderMessage *history, int n_history,
                     const char *model, const char *agent,
                     gboolean is_follow_up, GCancellable *cancellable,
                     ProviderCallback callback, gpointer callback_data,
                     GError **error);

  void (*cancel)(void *impl);
  void (*cancel_tab)(void *impl, gpointer tab_key);
  char *(*get_display_string)(void *impl, const char *query, const char *model,
                              const char *agent, gboolean is_follow_up);
  char **(*get_models)(void *impl);
  char **(*get_agents)(void *impl);
  gboolean needs_full_history;
} ProviderVTable;

typedef struct {
  ProviderVTable *vtable;
  void *impl;
} Provider;

#endif
