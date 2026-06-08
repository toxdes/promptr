#include "http.h"
#include <curl/curl.h>
#include <glib.h>
#include <string.h>

/* ── internal types ───────────────────────────────────────────── */

typedef struct {
  CURL *easy;
  struct curl_slist *header_list;
  GCancellable *cancellable;
  gulong cancel_id;
  volatile gboolean cancelled;
  GMainContext *context;
  HttpChunkCb chunk_cb;
  HttpDoneCb done_cb;
  gpointer user_data;
  char *url_copy; /* CURLOPT_URL doesn't copy — keep it alive */
} HttpStream;

typedef struct {
  HttpChunkCb cb;
  gpointer user;
  char *data;
} ChunkEvent;

typedef struct {
  HttpDoneCb cb;
  gpointer user;
  GError *error;
  long response_code;
  char *content_type;
} DoneEvent;

/* ── forward declarations ─────────────────────────────────────── */

static void http_stream_free(HttpStream *s);
static void on_cancelled(GCancellable *cancellable, gpointer user_data);
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
static int xfer_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow);
static void curl_thread(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable);
static gboolean dispatch_chunk(gpointer user);
static gboolean dispatch_done(gpointer user);

/* ── lifecycle ────────────────────────────────────────────────── */

static void http_stream_free(HttpStream *s) {
  if (s == NULL)
    return;
  if (s->cancel_id != 0 && s->cancellable != NULL)
    g_cancellable_disconnect(s->cancellable, s->cancel_id);
  if (s->header_list != NULL)
    curl_slist_free_all(s->header_list);
  if (s->easy != NULL)
    curl_easy_cleanup(s->easy);
  g_free(s->url_copy);
  g_free(s);
}

/* ── cancellation ─────────────────────────────────────────────── */

static void on_cancelled(GCancellable *cancellable, gpointer user_data) {
  HttpStream *s = user_data;

  (void)cancellable;
  s->cancelled = TRUE;
}

/* ── curl callbacks (called from thread) ──────────────────────── */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  HttpStream *s = userdata;
  gsize total = size * nmemb;

  if (s->cancelled)
    return 0;

  if (s->chunk_cb != NULL) {
    ChunkEvent *ev = g_new(ChunkEvent, 1);

    ev->cb = s->chunk_cb;
    ev->user = s->user_data;
    ev->data = g_strndup(ptr, total);
    g_main_context_invoke(s->context, dispatch_chunk, ev);
  }

  return total;
}

static int xfer_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow) {
  HttpStream *s = clientp;

  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  (void)ulnow;
  return s->cancelled ? 1 : 0;
}

/* ── thread entry ─────────────────────────────────────────────── */

static void curl_thread(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable) {
  HttpStream *s = task_data;
  CURLcode res;

  (void)task;
  (void)source_object;
  (void)cancellable;

  curl_easy_setopt(s->easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(s->easy, CURLOPT_WRITEDATA, s);
  curl_easy_setopt(s->easy, CURLOPT_XFERINFOFUNCTION, xfer_cb);
  curl_easy_setopt(s->easy, CURLOPT_XFERINFODATA, s);
  curl_easy_setopt(s->easy, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(s->easy, CURLOPT_POST, 1L);
  curl_easy_setopt(s->easy, CURLOPT_HTTPHEADER, s->header_list);
  curl_easy_setopt(s->easy, CURLOPT_TIMEOUT, 120L);

  res = curl_easy_perform(s->easy);

  /* Capture response metadata */
  {
    long response_code = 0;
    char *content_type = NULL;

    curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &response_code);
    {
      const char *ct = NULL;

      curl_easy_getinfo(s->easy, CURLINFO_CONTENT_TYPE, &ct);
      if (ct != NULL)
        content_type = g_strdup(ct);
    }

    if (res == CURLE_ABORTED_BY_CALLBACK && s->cancelled) {
      DoneEvent *ev = g_new(DoneEvent, 1);

      ev->cb = s->done_cb;
      ev->user = s->user_data;
      ev->error =
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ev->response_code = response_code;
      ev->content_type = content_type;
      g_main_context_invoke(s->context, dispatch_done, ev);
    } else if (res != CURLE_OK) {
      g_autofree char *msg =
          g_strdup_printf("HTTP request failed: %s", curl_easy_strerror(res));
      DoneEvent *ev = g_new(DoneEvent, 1);

      ev->cb = s->done_cb;
      ev->user = s->user_data;
      ev->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, msg);
      ev->response_code = response_code;
      ev->content_type = content_type;
      g_main_context_invoke(s->context, dispatch_done, ev);
    } else {
      DoneEvent *ev = g_new(DoneEvent, 1);

      ev->cb = s->done_cb;
      ev->user = s->user_data;
      ev->error = NULL;
      ev->response_code = response_code;
      ev->content_type = content_type;
      g_main_context_invoke(s->context, dispatch_done, ev);
    }
  }
}

/* ── main-thread dispatchers ──────────────────────────────────── */

static gboolean dispatch_chunk(gpointer user) {
  ChunkEvent *ev = user;

  if (ev->cb != NULL)
    ev->cb(ev->data, ev->data != NULL ? strlen(ev->data) : 0, ev->user);
  g_free(ev->data);
  g_free(ev);
  return G_SOURCE_REMOVE;
}

static gboolean dispatch_done(gpointer user) {
  DoneEvent *ev = user;

  if (ev->cb != NULL)
    ev->cb(ev->error, ev->response_code, ev->content_type, ev->user);
  if (ev->error != NULL)
    g_error_free(ev->error);
  g_free(ev->content_type);
  g_free(ev);
  return G_SOURCE_REMOVE;
}

/* ── public API ───────────────────────────────────────────────── */

void http_post_stream(const char *url, const char *const *headers,
                      const char *body, gsize body_len,
                      GCancellable *cancellable, HttpChunkCb chunk_cb,
                      HttpDoneCb done_cb, gpointer user_data) {
  HttpStream *s;

  s = g_new0(HttpStream, 1);
  s->context = g_main_context_get_thread_default();
  if (s->context == NULL)
    s->context = g_main_context_default();
  s->chunk_cb = chunk_cb;
  s->done_cb = done_cb;
  s->user_data = user_data;
  s->cancellable = cancellable;

  s->easy = curl_easy_init();
  s->url_copy = g_strdup(url);
  curl_easy_setopt(s->easy, CURLOPT_URL, s->url_copy);
  /* CURLOPT_COPYPOSTFIELDS makes curl copy the data internally —
     the caller can free body immediately after http_post_stream returns. */
  curl_easy_setopt(s->easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
  curl_easy_setopt(s->easy, CURLOPT_COPYPOSTFIELDS, body);
  curl_easy_setopt(s->easy, CURLOPT_USERAGENT, "Promptr/" VERSION);

  if (headers != NULL) {
    for (int i = 0; headers[i] != NULL; i++)
      s->header_list = curl_slist_append(s->header_list, headers[i]);
  }

  if (cancellable != NULL) {
    s->cancel_id =
        g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), s, NULL);
  }

  {
    GTask *task = g_task_new(NULL, cancellable, NULL, NULL);

    g_task_set_task_data(task, s, (GDestroyNotify)http_stream_free);
    g_task_run_in_thread(task, curl_thread);
    g_object_unref(task);
  }
}
