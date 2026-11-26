#ifndef PROVIDER_H
#define PROVIDER_H

#include "thread-store.h"
#include <gio/gio.h>
#include <glib.h>

typedef struct _RuntimeConfig RuntimeConfig;

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
  char **models;
  guint64 output_end;
} ProviderEvent;

typedef struct {
  const char *thread_id;
  const char *request_id;
  const char *query;
  const char *model;
  const char *agent_name;
  const char *agent_content;
  const char *project_dir;
  ThreadStore *store;
  gboolean follow_up;
  gboolean connection_test;
  gboolean discover_models;
} ProviderRequest;

typedef void (*ProviderCallback)(gpointer source, const ProviderEvent *event,
                                 gpointer user_data);

typedef struct _ProviderVTable {
  const char *name;

  void (*destroy)(void *impl);

  gboolean (*submit)(void *impl, gpointer tab_key,
                     const ProviderRequest *request, GCancellable *cancellable,
                     ProviderCallback callback, gpointer callback_data,
                     GError **error);

  void (*cancel)(void *impl);
  void (*cancel_tab)(void *impl, gpointer tab_key);
  char *(*get_display_string)(void *impl, const char *query, const char *model,
                              const char *agent, gboolean is_follow_up);
} ProviderVTable;

typedef struct {
  ProviderVTable *vtable;
  void *impl;
} Provider;

#endif
