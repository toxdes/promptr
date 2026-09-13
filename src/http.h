#ifndef HTTP_H
#define HTTP_H

#include <gio/gio.h>
#include <glib.h>

typedef void (*HttpChunkCb)(const char *data, gsize len, gpointer user);
typedef void (*HttpDoneCb)(GError *error, long response_code,
                           const char *content_type, gpointer user);

void http_post_stream(const char *url, const char *const *headers,
                      const char *body, gsize body_len,
                      GCancellable *cancellable, HttpChunkCb chunk_cb,
                      HttpDoneCb done_cb, gpointer user_data);

#endif
