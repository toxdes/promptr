#ifndef THREAD_STORE_H
#define THREAD_STORE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

typedef struct _ThreadStore ThreadStore;
typedef struct {
  char *text;
  gint64 next_message;
  guint64 before;
  gboolean more;
  gint stream_chars;
  guint64 stream_bytes;
} ThreadPage;

ThreadStore *thread_store_new(const char *id);
ThreadStore *thread_store_ref(ThreadStore *store);
void thread_store_unref(ThreadStore *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ThreadStore, thread_store_unref)

/* Blocking disk operations: call from a worker, never from a GTK callback. */
gboolean thread_store_import(ThreadStore *store, GError **error);
gboolean thread_store_begin(ThreadStore *store, const char *request_id,
                            const char *query, gboolean fresh, GError **error);
gboolean thread_store_chunk(ThreadStore *store, const char *text,
                            GError **error);
gboolean thread_store_finish(ThreadStore *store, const char *status,
                             GError **error);
gboolean thread_store_has_unsaved(ThreadStore *store);
JsonArray *thread_store_messages(ThreadStore *store, GError **error);
ThreadPage *thread_store_page(ThreadStore *store, gint64 message,
                              guint64 before, GError **error);
void thread_page_free(ThreadPage *page);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ThreadPage, thread_page_free)
char *thread_store_get_resume(ThreadStore *store, const char *profile);
gboolean thread_store_set_resume(ThreadStore *store, const char *profile,
                                 const char *state, GError **error);
gboolean thread_store_delete(const char *id, GError **error);
char *thread_store_directory(void);
gboolean thread_store_is_deleting(const char *id);
gboolean thread_store_reserve_delete(const char *id);
void thread_store_release_delete(const char *id);

#endif
