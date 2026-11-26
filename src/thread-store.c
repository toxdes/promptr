#define _POSIX_C_SOURCE 200809L
#include "thread-store.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PAGE_BYTES    (256u * 1024u)
#define PAGE_MESSAGES 100u
#define PAGE_TURNS    50u

typedef struct {
  guint64 start, end, bytes;
  gboolean assistant;
  gboolean system;
  guint status;
} MessageIndex;

struct _ThreadStore {
  gint refs;
  GMutex mutex;
  char *id, *path, *index_path, *resume_path;
  GArray *index;
  guint64 size;
  gboolean loaded;
  GString *unsaved;
  gboolean active;
};

char *thread_store_directory(void) {
  return g_build_filename(g_get_user_data_dir(), DATA_DIR_SUFFIX, "tabs", NULL);
}

static char *store_path(const char *id, const char *suffix) {
  g_autofree char *dir = thread_store_directory();
  g_autofree char *name = g_strconcat(id, suffix, NULL);
  return g_build_filename(dir, name, NULL);
}

ThreadStore *thread_store_new(const char *id) {
  if (id == NULL || !g_uuid_string_is_valid(id))
    return NULL;
  ThreadStore *s = g_new0(ThreadStore, 1);
  s->refs = 1;
  g_mutex_init(&s->mutex);
  s->id = g_strdup(id);
  s->path = store_path(id, ".journal");
  s->index_path = store_path(id, ".index");
  s->resume_path = store_path(id, ".resume");
  s->index = g_array_new(FALSE, FALSE, sizeof(MessageIndex));
  return s;
}

ThreadStore *thread_store_ref(ThreadStore *s) {
  if (s != NULL)
    g_atomic_int_inc(&s->refs);
  return s;
}

void thread_store_unref(ThreadStore *s) {
  if (s == NULL || !g_atomic_int_dec_and_test(&s->refs))
    return;
  g_mutex_clear(&s->mutex);
  g_free(s->id);
  g_free(s->path);
  g_free(s->index_path);
  g_free(s->resume_path);
  g_array_unref(s->index);
  if (s->unsaved != NULL)
    g_string_free(s->unsaved, TRUE);
  g_free(s);
}

static gboolean io_error(GError **error, const char *action) {
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s", action,
              g_strerror(errno));
  return FALSE;
}

static gboolean atomic_write(const char *path, const char *data, gsize len,
                             GError **error) {
  return g_file_set_contents_full(path, data, len,
                                  G_FILE_SET_CONTENTS_CONSISTENT |
                                      G_FILE_SET_CONTENTS_DURABLE,
                                  0600, error);
}

static guint terminal_status(const char *status) {
  if (g_strcmp0(status, "completed") == 0 || g_strcmp0(status, "imported") == 0)
    return 1;
  if (g_strcmp0(status, "cancelled") == 0)
    return 2;
  if (g_strcmp0(status, "failed") == 0)
    return 4;
  return 3;
}

static void cache_index(ThreadStore *s) {
  g_autoptr(GString) body = g_string_new(NULL);
  for (guint i = 0; i < s->index->len; i++) {
    MessageIndex *m = &g_array_index(s->index, MessageIndex, i);
    g_string_append_printf(body,
                           "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                           " %" G_GUINT64_FORMAT " %u %u\n",
                           m->start, m->end, m->bytes,
                           m->system      ? 2
                           : m->assistant ? 1
                                          : 0,
                           m->status);
  }
  g_autofree char *hash =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, body->str, body->len);
  g_autofree char *data =
      g_strdup_printf("promptr-index-4 %" G_GUINT64_FORMAT " %u %s\n%s",
                      s->size, s->index->len, hash, body->str);
  /* Disposable cache: write failures cannot lose canonical history. */
  g_file_set_contents_full(s->index_path, data, -1,
                           G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

static gboolean load_cached_index(ThreadStore *s, const GStatBuf *journal) {
  GStatBuf cached;
  if (g_stat(s->index_path, &cached) != 0 ||
      cached.st_mtim.tv_sec < journal->st_mtim.tv_sec ||
      (cached.st_mtim.tv_sec == journal->st_mtim.tv_sec &&
       cached.st_mtim.tv_nsec < journal->st_mtim.tv_nsec))
    return FALSE;
  g_autofree char *data = NULL;
  g_auto(GStrv) lines = NULL;
  guint64 cached_size;
  guint count;
  char expected[65] = {0};
  if (!g_file_get_contents(s->index_path, &data, NULL, NULL) ||
      sscanf(data, "promptr-index-4 %" G_GUINT64_FORMAT " %u %64s",
             &cached_size, &count, expected) != 3 ||
      cached_size != (guint64)journal->st_size)
    return FALSE;
  char *body = strchr(data, '\n');
  if (body == NULL)
    return FALSE;
  body++;
  g_autofree char *hash =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, body, -1);
  if (g_strcmp0(expected, hash) != 0)
    return FALSE;
  lines = g_strsplit(body, "\n", -1);
  guint64 previous = 0;
  for (guint i = 0; lines[i] != NULL && *lines[i] != '\0'; i++) {
    MessageIndex m = {0};
    guint role;
    if (sscanf(lines[i],
               "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
               " %u %u",
               &m.start, &m.end, &m.bytes, &role, &m.status) != 5 ||
        m.start < previous || m.end < m.start || m.end > cached_size ||
        role > 2 || m.status > 4) {
      g_array_set_size(s->index, 0);
      return FALSE;
    }
    m.assistant = role == 1;
    m.system = role == 2;
    previous = m.end;
    g_array_append_val(s->index, m);
  }
  if (s->index->len != count) {
    g_array_set_size(s->index, 0);
    return FALSE;
  }
  s->size = cached_size;
  return TRUE;
}

/* A damaged suffix is retained separately before replacing the journal with its
 * valid prefix. */
static gboolean recover_tail(ThreadStore *s, guint64 valid, GError **error) {
  g_autofree char *backup = g_strdup_printf("%s.damaged-%" G_GINT64_FORMAT,
                                            s->path, g_get_real_time());
  g_autoptr(GFile) source = g_file_new_for_path(s->path);
  g_autoptr(GFile) destination = g_file_new_for_path(backup);
  if (!g_file_copy(source, destination, G_FILE_COPY_NONE, NULL, NULL, NULL,
                   error))
    return FALSE;
  g_autofree char *temporary = g_strconcat(s->path, ".recover-XXXXXX", NULL);
  int output = g_mkstemp_full(temporary, O_RDWR, 0600);
  if (output < 0)
    return io_error(error, "Recover conversation");
  FILE *input = g_fopen(backup, "r");
  gboolean ok = input != NULL;
  guint64 copied = 0;
  char buffer[65536];
  while (ok && copied < valid) {
    gsize count = MIN((guint64)sizeof(buffer), valid - copied);
    if (fread(buffer, 1, count, input) != count) {
      ok = FALSE;
      break;
    }
    gsize offset = 0;
    while (offset < count) {
      ssize_t written = write(output, buffer + offset, count - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0) {
        ok = FALSE;
        break;
      }
      offset += written;
    }
    copied += count;
  }
  if (input != NULL)
    fclose(input);
  if (ok)
    ok = fsync(output) == 0;
  if (close(output) != 0)
    ok = FALSE;
  if (ok)
    ok = g_rename(temporary, s->path) == 0;
  if (!ok) {
    g_unlink(temporary);
    return io_error(error, "Recover conversation");
  }
  g_warning(
      "Recovered interrupted conversation journal; original retained at %s",
      backup);
  return TRUE;
}

/* Stop allocating as soon as the on-disk record limit is reached. */
static ssize_t bounded_record(char **line, size_t *capacity, FILE *file) {
  g_autoptr(GString) record = g_string_new(NULL);
  int ch;
  (void)capacity;
  while ((ch = fgetc(file)) != EOF) {
    if (record->len >= PROMPTR_MAX_FRAME) {
      g_free(*line);
      *line = NULL;
      return -2;
    }
    g_string_append_c(record, ch);
    if (ch == '\n')
      break;
  }
  if (record->len == 0)
    return -1;
  g_free(*line);
  ssize_t length = record->len;
  *line = g_string_free(g_steal_pointer(&record), FALSE);
  return length;
}

static gboolean load_index(ThreadStore *s, GError **error) {
  if (s->loaded)
    return TRUE;
  g_array_set_size(s->index, 0);
  GStatBuf statbuf;
  if (g_stat(s->path, &statbuf) != 0) {
    if (errno == ENOENT) {
      s->loaded = TRUE;
      return TRUE;
    }
    return io_error(error, "Read conversation");
  }
  if (load_cached_index(s, &statbuf)) {
    s->loaded = TRUE;
    return TRUE;
  }
  FILE *file = g_fopen(s->path, "r");
  if (file == NULL)
    return io_error(error, "Open conversation");
  char *line = NULL;
  size_t capacity = 0;
  ssize_t n;
  guint64 valid = 0;
  gboolean damaged = FALSE;
  while ((n = bounded_record(&line, &capacity, file)) >= 0) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (n == 0 || line[n - 1] != '\n' || (gsize)n > PROMPTR_MAX_FRAME ||
        !json_parser_load_from_data(parser, line, n, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
      damaged = TRUE;
      break;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    const char *kind = protocol_string(o, "kind");
    const char *text = protocol_string(o, "content");
    if (g_strcmp0(kind, "message") == 0 || g_strcmp0(kind, "assistant") == 0) {
      MessageIndex m = {valid,
                        valid + n,
                        text != NULL ? strlen(text) : 0,
                        g_strcmp0(protocol_string(o, "role"), "assistant") ==
                                0 ||
                            g_strcmp0(kind, "assistant") == 0,
                        g_strcmp0(protocol_string(o, "role"), "system") == 0,
                        g_strcmp0(kind, "message") == 0 ? 1 : 0};
      g_array_append_val(s->index, m);
    } else if (g_strcmp0(kind, "delta") == 0 && s->index->len > 0 &&
               text != NULL) {
      MessageIndex *m =
          &g_array_index(s->index, MessageIndex, s->index->len - 1);
      m->bytes += strlen(text);
      m->end = valid + n;
    } else if (g_strcmp0(kind, "finish") == 0 && s->index->len > 0) {
      MessageIndex *m =
          &g_array_index(s->index, MessageIndex, s->index->len - 1);
      m->status = terminal_status(text);
    } else if (g_strcmp0(kind, "finish") != 0) {
      damaged = TRUE;
      break;
    }
    valid += n;
  }
  if (n == -2)
    damaged = TRUE;
  gboolean failed = ferror(file);
  g_free(line);
  fclose(file);
  if (failed)
    return io_error(error, "Read conversation");
  if (damaged && !recover_tail(s, valid, error))
    return FALSE;
  s->size = valid;
  s->loaded = TRUE;
  cache_index(s);
  return TRUE;
}

static gboolean append_record(ThreadStore *s, const char *kind,
                              const char *role, const char *text,
                              GError **error) {
  if (text != NULL && strlen(text) > PROMPTR_MAX_DELTA) {
    const char *cursor = text;
    gsize remaining = strlen(text);
    const char *record_kind = kind;
    while (*cursor) {
      gsize count = MIN(remaining, PROMPTR_MAX_DELTA);
      while (count > 0 && cursor[count] &&
             ((guchar)cursor[count] & 0xc0) == 0x80)
        count--;
      if (count == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Invalid UTF-8 conversation content");
        return FALSE;
      }
      g_autofree char *part = g_strndup(cursor, count);
      if (!append_record(s, record_kind, role, part, error))
        return FALSE;
      cursor += count;
      remaining -= count;
      record_kind = "delta";
    }
    return TRUE;
  }
  g_autoptr(JsonObject) o = json_object_new();
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_object_set_string_member(o, "kind", kind);
  if (role != NULL)
    json_object_set_string_member(o, "role", role);
  if (text != NULL)
    json_object_set_string_member(o, "content", text);
  json_node_set_object(node, o);
  g_autofree char *json = json_to_string(node, FALSE);
  if (s->unsaved != NULL) {
    g_string_append_printf(s->unsaved, "%s\n", json);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Conversation has unsaved output; free disk space and "
                        "choose Latest to retry saving");
    return FALSE;
  }
  FILE *f = g_fopen(s->path, "a");
  if (f == NULL) {
    s->unsaved = g_string_new(json);
    g_string_append_c(s->unsaved, '\n');
    return io_error(error, "Save conversation");
  }
  gsize len = strlen(json) + 1;
  gboolean ok = fprintf(f, "%s\n", json) >= 0;
  if (ok && g_strcmp0(kind, "finish") == 0)
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (fclose(f) != 0)
    ok = FALSE;
  if (!ok) {
    s->unsaved = g_string_new(json);
    g_string_append_c(s->unsaved, '\n');
    return io_error(error, "Save conversation");
  }
  if (g_strcmp0(kind, "message") == 0 || g_strcmp0(kind, "assistant") == 0) {
    MessageIndex m = {s->size,
                      s->size + len,
                      text != NULL ? strlen(text) : 0,
                      g_strcmp0(role, "assistant") == 0,
                      g_strcmp0(role, "system") == 0,
                      g_strcmp0(kind, "message") == 0 ? 1 : 0};
    g_array_append_val(s->index, m);
  } else if (g_strcmp0(kind, "delta") == 0 && s->index->len > 0 &&
             text != NULL) {
    MessageIndex *m = &g_array_index(s->index, MessageIndex, s->index->len - 1);
    m->bytes += strlen(text);
    m->end = s->size + len;
  } else if (g_strcmp0(kind, "finish") == 0 && s->index->len > 0) {
    MessageIndex *m = &g_array_index(s->index, MessageIndex, s->index->len - 1);
    m->status = terminal_status(text);
  }
  s->size += len;
  return TRUE;
}

gboolean thread_store_import(ThreadStore *s, GError **error) {
  g_autofree char *dir = thread_store_directory();
  if (g_mkdir_with_parents(dir, 0700) != 0)
    return io_error(error, "Create conversation directory");
  if (g_file_test(s->path, G_FILE_TEST_EXISTS))
    return TRUE;
  g_autofree char *conf = store_path(s->id, ".conf");
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autoptr(GError) legacy_error = NULL;
  if (!g_key_file_load_from_file(kf, conf, G_KEY_FILE_NONE, &legacy_error)) {
    if (g_file_test(conf, G_FILE_TEST_EXISTS)) {
      g_propagate_error(error, g_steal_pointer(&legacy_error));
      return FALSE;
    }
    return atomic_write(s->path, "", 0, error);
  }
  /* Import into a temporary journal and publish atomically. Legacy files remain
   * untouched. */
  g_autofree char *target = s->path;
  s->path = g_strconcat(target, ".import", NULL);
  gboolean ok = atomic_write(s->path, "", 0, error);
  int version = g_key_file_get_integer(kf, "tab", "version", NULL);
  int count = g_key_file_get_integer(kf, "messages", "count", NULL);
  if (version < 2)
    count = g_key_file_get_integer(kf, "follow_up", "turn", NULL);
  for (int i = 0; ok && i < count; i++) {
    g_autofree char *group =
        g_strdup_printf(version >= 2 ? "msg_%d" : "history_%d",
                        version >= 2 ? i : count - 1 - i);
    g_autofree char *content = g_key_file_get_string(
        kf, group, version >= 2 ? "content" : "query", NULL);
    g_autofree char *role = version >= 2
                                ? g_key_file_get_string(kf, group, "role", NULL)
                                : g_strdup("user");
    if (content != NULL)
      ok = append_record(s, "message", role != NULL ? role : "user", content,
                         error);
    if (version < 2 && ok) {
      g_autofree char *output =
          g_key_file_get_string(kf, group, "output", NULL);
      if (output != NULL)
        ok = append_record(s, "message", "assistant", output, error);
    }
  }
  if (ok && version < 2) {
    g_autofree char *query =
        g_key_file_get_string(kf, "follow_up", "last_query", NULL);
    g_autofree char *output =
        g_key_file_get_string(kf, "follow_up", "last_output", NULL);
    if (query != NULL && *query)
      ok = append_record(s, "message", "user", query, error);
    if (ok && output != NULL && *output)
      ok = append_record(s, "message", "assistant", output, error);
  }
  const char *import_status = "imported";
  if (ok &&
      (s->index->len == 0 ||
       !g_array_index(s->index, MessageIndex, s->index->len - 1).assistant)) {
    g_autofree char *txt = store_path(s->id, ".txt");
    g_autofree char *content = NULL;
    if (g_file_get_contents(txt, &content, NULL, NULL) && *content) {
      if (s->index->len > 0) {
        char *history = strstr(content, "\n---\nQ.");
        if (history != NULL)
          *history = '\0';
        import_status = "interrupted";
      }
      if (*content)
        ok = append_record(s, "message", "assistant", content, error);
    }
  }
  if (ok)
    ok = append_record(s, "finish", NULL, import_status, error);
  if (ok && g_rename(s->path, target) != 0)
    ok = io_error(error, "Publish migrated conversation");
  g_free(s->path);
  s->path = g_steal_pointer(&target);
  s->loaded = ok;
  if (!ok) {
    g_array_set_size(s->index, 0);
    s->size = 0;
    if (s->unsaved != NULL) {
      g_string_free(s->unsaved, TRUE);
      s->unsaved = NULL;
    }
  }
  if (ok)
    cache_index(s);
  return ok;
}

gboolean thread_store_begin(ThreadStore *s, const char *request_id,
                            const char *query, gboolean fresh, GError **error) {
  g_mutex_lock(&s->mutex);
  if (s->unsaved != NULL) {
    g_mutex_unlock(&s->mutex);
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Save the pending output before starting another request");
    return FALSE;
  }
  gboolean ok = thread_store_import(s, error) && load_index(s, error);
  if (ok && fresh) {
    ok = atomic_write(s->path, "", 0, error);
    if (ok) {
      g_array_set_size(s->index, 0);
      s->size = 0;
      ok = atomic_write(s->resume_path, "{}", 2, error);
    }
  }
  if (ok)
    ok = append_record(s, "message", "user", query, error);
  if (ok)
    ok = append_record(s, "assistant", "assistant", NULL, error);
  s->active = ok;
  (void)request_id;
  g_mutex_unlock(&s->mutex);
  return ok;
}

gboolean thread_store_chunk(ThreadStore *s, const char *text, GError **error) {
  g_mutex_lock(&s->mutex);
  gboolean ok = append_record(s, "delta", NULL, text, error);
  g_mutex_unlock(&s->mutex);
  return ok;
}

gboolean thread_store_finish(ThreadStore *s, const char *status,
                             GError **error) {
  g_mutex_lock(&s->mutex);
  gboolean ok = append_record(s, "finish", NULL, status, error);
  s->active = FALSE;
  if (s->index->len > 0) {
    MessageIndex *m = &g_array_index(s->index, MessageIndex, s->index->len - 1);
    m->status = terminal_status(status);
  }
  if (ok)
    cache_index(s);
  g_mutex_unlock(&s->mutex);
  return ok;
}

gboolean thread_store_has_unsaved(ThreadStore *s) {
  if (s == NULL)
    return FALSE;
  g_mutex_lock(&s->mutex);
  gboolean pending = s->unsaved != NULL;
  g_mutex_unlock(&s->mutex);
  return pending;
}

static gboolean retry_unsaved(ThreadStore *s, GError **error) {
  if (s->unsaved == NULL)
    return TRUE;
  /* The old valid prefix and failed records remain in memory until an atomic,
   * durable replacement succeeds. Never append after a partially written line.
   */
  g_autofree char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(s->path, &data, &length, error))
    return FALSE;
  if (length < s->size) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Conversation changed while output was unsaved");
    return FALSE;
  }
  g_autoptr(GString) recovered = g_string_new_len(data, s->size);
  g_string_append_len(recovered, s->unsaved->str, s->unsaved->len);
  if (!atomic_write(s->path, recovered->str, recovered->len, error))
    return FALSE;
  g_string_free(s->unsaved, TRUE);
  s->unsaved = NULL;
  s->loaded = FALSE;
  g_array_set_size(s->index, 0);
  return load_index(s, error);
}

static char *read_message(ThreadStore *s, MessageIndex *m, guint64 before,
                          gsize cap, GError **error) {
  FILE *f = g_fopen(s->path, "r");
  if (f == NULL) {
    io_error(error, "Open conversation");
    return NULL;
  }
  if (fseek(f, m->start, SEEK_SET) != 0) {
    fclose(f);
    io_error(error, "Read conversation");
    return NULL;
  }
  g_autoptr(GString) text = g_string_new(NULL);
  char *line = NULL;
  size_t capacity = 0;
  guint64 position = 0;
  guint64 stop = before > 0 ? MIN(before, m->bytes) : m->bytes;
  guint64 start = stop > cap ? stop - cap : 0;
  ssize_t record_length = 0;
  while ((guint64)ftell(f) < m->end &&
         (record_length = bounded_record(&line, &capacity, f)) > 0) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, error)) {
      g_free(line);
      fclose(f);
      return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
      g_free(line);
      fclose(f);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Invalid conversation record");
      return NULL;
    }
    const char *content =
        protocol_string(json_node_get_object(root), "content");
    if (content == NULL)
      continue;
    guint64 len = strlen(content), end = MIN(position + len, stop);
    if (end > MAX(start, position))
      g_string_append_len(text, content + MAX(start, position) - position,
                          end - MAX(start, position));
    position += len;
  }
  gboolean failed = ferror(f);
  if (record_length == -2) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Conversation record exceeds limit");
    g_free(line);
    fclose(f);
    return NULL;
  }
  g_free(line);
  fclose(f);
  if (failed) {
    io_error(error, "Read conversation");
    return NULL;
  }
  /* A page may begin in the middle of a UTF-8 character. */
  while (text->len > 0 && (((guchar)text->str[0] & 0xc0) == 0x80))
    g_string_erase(text, 0, 1);
  while (text->len > 0 && !g_utf8_validate(text->str, text->len, NULL))
    g_string_truncate(text, text->len - 1);
  return g_string_free(g_steal_pointer(&text), FALSE);
}

JsonArray *thread_store_messages(ThreadStore *s, GError **error) {
  g_mutex_lock(&s->mutex);
  if (!thread_store_import(s, error) || !load_index(s, error)) {
    g_mutex_unlock(&s->mutex);
    return NULL;
  }
  JsonArray *messages = json_array_new();
  guint64 total = 0;
  for (guint i = 0; i < s->index->len; i++) {
    MessageIndex *m = &g_array_index(s->index, MessageIndex, i);
    total += m->bytes;
    if (total > PROMPTR_MAX_FRAME) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
          "Conversation exceeds IPC limit; start a fresh conversation");
      json_array_unref(messages);
      g_mutex_unlock(&s->mutex);
      return NULL;
    }
    g_autofree char *text = read_message(s, m, 0, PROMPTR_MAX_FRAME, error);
    if (text == NULL) {
      json_array_unref(messages);
      g_mutex_unlock(&s->mutex);
      return NULL;
    }
    if (*text == '\0' && m->assistant)
      continue;
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "role",
                                  m->system      ? "system"
                                  : m->assistant ? "assistant"
                                                 : "user");
    json_object_set_string_member(o, "content", text);
    json_array_add_object_element(messages, o);
  }
  g_mutex_unlock(&s->mutex);
  return messages;
}

ThreadPage *thread_store_page(ThreadStore *s, gint64 message, guint64 before,
                              GError **error) {
  g_mutex_lock(&s->mutex);
  if (!retry_unsaved(s, error) || !thread_store_import(s, error) ||
      !load_index(s, error)) {
    g_mutex_unlock(&s->mutex);
    return NULL;
  }
  ThreadPage *page = g_new0(ThreadPage, 1);
  page->stream_chars = -1;
  g_autoptr(GString) out = g_string_new(NULL);
  gint64 i = message < 0 ? (gint64)s->index->len - 1
                         : MIN(message, (gint64)s->index->len - 1);
  gint64 latest_user = -1, latest_answer = -1;
  guint ordinal = 0;
  for (guint n = 0; n < s->index->len; n++) {
    MessageIndex *entry = &g_array_index(s->index, MessageIndex, n);
    if (!entry->assistant && !entry->system) {
      latest_user = n;
      latest_answer = -1;
      if ((gint64)n <= i)
        ordinal++;
    } else if (entry->assistant && (gint64)n > latest_user) {
      latest_answer = n;
    }
  }
  guint count = 0, turns = 0;
  for (; i >= 0 && count < PAGE_MESSAGES && turns < PAGE_TURNS &&
         out->len + 128 < PAGE_BYTES;
       i--) {
    MessageIndex *slot = &g_array_index(s->index, MessageIndex, i);
    gboolean user_slot = !slot->assistant && !slot->system;
    if (i == latest_user) {
      if (ordinal > 0)
        ordinal--;
      continue;
    }
    /* Reverse the order of complete turns, while keeping each older question
     * before its answer. The cursor addresses this display slot, so partial
     * messages and page boundaries can resume without losing any bytes. */
    gint64 content_index = i;
    if (i != latest_answer && slot->assistant && i > 0) {
      MessageIndex *previous = &g_array_index(s->index, MessageIndex, i - 1);
      if (!previous->assistant && !previous->system && i - 1 != latest_user)
        content_index = i - 1;
    } else if (user_slot && i + 1 < (gint64)s->index->len) {
      MessageIndex *next = &g_array_index(s->index, MessageIndex, i + 1);
      if (next->assistant)
        content_index = i + 1;
    }
    MessageIndex *m = &g_array_index(s->index, MessageIndex, content_index);
    if (!m->assistant && !m->system)
      turns++;
    gsize cap = PAGE_BYTES - out->len - 128;
    g_autofree char *text = read_message(s, m, before, cap, error);
    if (text == NULL) {
      thread_page_free(page);
      g_mutex_unlock(&s->mutex);
      return NULL;
    }
    guint64 stop = before > 0 ? before : m->bytes;
    gboolean newest = content_index == latest_answer;
    if (newest) {
      if (before)
        g_string_append(out, "[earlier portion]\n");
    } else if (m->system) {
      g_string_append(out, "\n---\nContext: ");
    } else if (!m->assistant) {
      g_string_append_printf(out, "\n---\nQ.%u: ", ordinal);
    } else {
      g_string_append(out, out->len > 0 ? "\nA: " : "A: ");
    }
    g_string_append(out, text);
    if (newest && s->active) {
      page->stream_chars = g_utf8_strlen(out->str, out->len);
      page->stream_bytes = m->bytes;
    }
    const char *status =
        m->assistant && m->status == 4   ? "failed"
        : m->assistant && m->status == 2 ? "cancelled"
        : m->assistant && (m->status == 3 || (m->status == 0 && !s->active))
            ? "interrupted"
            : NULL;
    if (status != NULL)
      g_string_append_printf(out, "\n[%s]", status);
    count++;
    before = 0;
    if (strlen(text) < stop) {
      page->before = stop - strlen(text);
      page->next_message = i;
      page->more = TRUE;
      break;
    }
    if (user_slot && ordinal > 0)
      ordinal--;
  }
  if (!page->more) {
    page->next_message = i;
    page->more = i >= 0;
  }
  page->text = g_string_free(g_steal_pointer(&out), FALSE);
  g_mutex_unlock(&s->mutex);
  return page;
}

void thread_page_free(ThreadPage *page) {
  if (page != NULL) {
    g_free(page->text);
    g_free(page);
  }
}

char *thread_store_get_resume(ThreadStore *s, const char *profile) {
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_file(parser, s->resume_path, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    return NULL;
  return g_strdup(protocol_string(
      json_node_get_object(json_parser_get_root(parser)), profile));
}

gboolean thread_store_set_resume(ThreadStore *s, const char *profile,
                                 const char *state, GError **error) {
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(JsonNode) node = NULL;
  if (json_parser_load_from_file(parser, s->resume_path, NULL) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    node = json_node_copy(json_parser_get_root(parser));
  else {
    node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, json_object_new());
  }
  JsonObject *o = json_node_get_object(node);
  if (state != NULL)
    json_object_set_string_member(o, profile, state);
  else
    json_object_remove_member(o, profile);
  g_autofree char *json = json_to_string(node, FALSE);
  return atomic_write(s->resume_path, json, strlen(json), error);
}

gboolean thread_store_delete(const char *id, GError **error) {
  if (id == NULL || !g_uuid_string_is_valid(id))
    return FALSE;
  const char *suffixes[] = {".journal", ".index",  ".resume", ".conf",
                            ".txt",     ".prompt", ".meta",   ".marks"};
  for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
    g_autofree char *path = store_path(id, suffixes[i]);
    if (g_unlink(path) != 0 && errno != ENOENT)
      return io_error(error, "Delete conversation");
  }
  g_autofree char *dir = thread_store_directory();
  g_autoptr(GDir) directory = g_dir_open(dir, 0, error);
  if (directory == NULL)
    return FALSE;
  g_autofree char *damaged = g_strconcat(id, ".journal.damaged-", NULL);
  g_autofree char *recover = g_strconcat(id, ".journal.recover-", NULL);
  g_autofree char *import = g_strconcat(id, ".journal.import", NULL);
  const char *name;
  while ((name = g_dir_read_name(directory)) != NULL) {
    if (!g_str_has_prefix(name, damaged) && !g_str_has_prefix(name, recover) &&
        g_strcmp0(name, import) != 0)
      continue;
    g_autofree char *path = g_build_filename(dir, name, NULL);
    if (g_unlink(path) != 0 && errno != ENOENT)
      return io_error(error, "Delete conversation recovery file");
  }
  return TRUE;
}

G_LOCK_DEFINE_STATIC(deleting_threads);
static GHashTable *deleting_threads;

gboolean thread_store_is_deleting(const char *id) {
  if (id == NULL || !g_uuid_string_is_valid(id))
    return FALSE;
  G_LOCK(deleting_threads);
  gboolean found =
      deleting_threads != NULL && g_hash_table_contains(deleting_threads, id);
  G_UNLOCK(deleting_threads);
  return found;
}
gboolean thread_store_reserve_delete(const char *id) {
  if (id == NULL || !g_uuid_string_is_valid(id))
    return FALSE;
  G_LOCK(deleting_threads);
  if (deleting_threads == NULL)
    deleting_threads =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean added = !g_hash_table_contains(deleting_threads, id);
  if (added)
    g_hash_table_add(deleting_threads, g_strdup(id));
  G_UNLOCK(deleting_threads);
  return added;
}
void thread_store_release_delete(const char *id) {
  if (id == NULL)
    return;
  G_LOCK(deleting_threads);
  if (deleting_threads != NULL) {
    g_hash_table_remove(deleting_threads, id);
    if (g_hash_table_size(deleting_threads) == 0)
      g_clear_pointer(&deleting_threads, g_hash_table_unref);
  }
  G_UNLOCK(deleting_threads);
}
