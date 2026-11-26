#define _POSIX_C_SOURCE 200809L
#include "plugin-runner.h"
#include "../lib/promptr-protocol/jsonrpc.h"
#include "agent-context.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef PROMPTR_STARTUP_SECONDS
#define PROMPTR_STARTUP_SECONDS 10
#endif
#ifndef PROMPTR_CANCEL_SECONDS
#define PROMPTR_CANCEL_SECONDS 5
#endif
#ifndef PROMPTR_TEST_SECONDS
#define PROMPTR_TEST_SECONDS 60
#endif
#ifndef PROMPTR_SHUTDOWN_SECONDS
#define PROMPTR_SHUTDOWN_SECONDS 5
#endif
#define EVENT_QUEUE_BYTES (1024u * 1024u)

typedef struct {
  gint refs;
  ProviderProfile *profile;
  GCancellable *cancel;
  gboolean busy;
  GMutex mutex;
  GCond drained;
  gsize queued;
  gpointer source;
  ProviderCallback callback;
  gpointer callback_data;
} Runner;

typedef struct {
  Runner *runner;
  ProviderRequest request;
  char *thread_id, *request_id, *query, *model, *agent_name, *agent_content,
      *project_dir;
} Job;

typedef struct {
  Runner *runner;
  ProviderEvent event;
  gsize bytes;
} Delivery;

typedef struct {
  Runner *runner;
  GPid pid;
  int input, output;
  GString *buffer;
  gint64 deadline;
  gboolean cancelling;
  const ProviderRequest *request;
} Transport;

static Runner *runner_ref(Runner *r) {
  g_atomic_int_inc(&r->refs);
  return r;
}
static void runner_unref(Runner *r) {
  if (!g_atomic_int_dec_and_test(&r->refs))
    return;
  provider_profile_free(r->profile);
  g_clear_object(&r->cancel);
  g_mutex_clear(&r->mutex);
  g_cond_clear(&r->drained);
  g_free(r);
}

static gboolean deliver(gpointer data) {
  Delivery *d = data;
  Runner *r = d->runner;
  if (d->event.type != PROVIDER_EVENT_CHUNK)
    r->busy = FALSE;
  if (r->callback != NULL)
    r->callback(r->source, &d->event, r->callback_data);
  g_mutex_lock(&r->mutex);
  r->queued -= d->bytes;
  g_cond_broadcast(&r->drained);
  g_mutex_unlock(&r->mutex);
  g_free(d->event.output);
  g_free(d->event.error_msg);
  g_strfreev(d->event.models);
  runner_unref(r);
  g_free(d);
  return G_SOURCE_REMOVE;
}

static void post_event(Runner *r, ProviderEventType type, const char *text,
                       const char *error, gint64 elapsed, char **models,
                       guint64 output_end) {
  if (type == PROVIDER_EVENT_CHUNK && g_cancellable_is_cancelled(r->cancel))
    return;
  gsize bytes = (text != NULL ? strlen(text) : 0) + 1;
  g_mutex_lock(&r->mutex);
  while (r->queued + bytes > EVENT_QUEUE_BYTES &&
         !g_cancellable_is_cancelled(r->cancel))
    g_cond_wait_until(&r->drained, &r->mutex,
                      g_get_monotonic_time() + G_TIME_SPAN_SECOND);
  if (type == PROVIDER_EVENT_CHUNK && g_cancellable_is_cancelled(r->cancel)) {
    g_mutex_unlock(&r->mutex);
    return;
  }
  r->queued += bytes;
  g_mutex_unlock(&r->mutex);
  Delivery *d = g_new0(Delivery, 1);
  d->runner = runner_ref(r);
  d->bytes = bytes;
  d->event.type = type;
  d->event.output = g_strdup(text);
  if (error != NULL) {
    gsize length = MIN(strlen(error), (gsize)4096);
    while (length > 0 && !g_utf8_validate(error, length, NULL))
      length--;
    d->event.error_msg = g_strndup(error, length);
  }
  d->event.elapsed_us = elapsed;
  d->event.models = g_strdupv(models);
  d->event.output_end = output_end;
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, deliver, d, NULL);
  g_source_attach(source, g_main_context_default());
  g_source_unref(source);
}

static gboolean transport_error(GError **error, const char *message) {
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
  return FALSE;
}

/* Writes happen only in this worker. Nonblocking descriptors prevent a stalled
 * plugin from trapping cancellation or shutdown inside a pipe write. */
static gboolean write_frame(Transport *t, const char *json, GError **error) {
  gsize size = strlen(json);
  if (size > PROMPTR_MAX_FRAME)
    return transport_error(error, "Request exceeds the 64 MiB IPC frame limit");
  gsize offset = 0;
  while (offset < size + 1) {
    if (t->deadline > 0 && g_get_monotonic_time() >= t->deadline)
      return transport_error(
          error, "Provider timed out; check its configuration and test again");
    if (g_cancellable_is_cancelled(t->runner->cancel))
      return transport_error(error,
                             "Request cancelled while writing to provider");
    struct pollfd fd = {t->input, POLLOUT, 0};
    int ready = poll(&fd, 1, 50);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
      return transport_error(error, "Provider closed its input");
    if (ready == 0)
      continue;
    const char *part = offset < size ? json + offset : "\n";
    gsize count = offset < size ? MIN((gsize)65536, size - offset) : 1;
    ssize_t n = write(t->input, part, count);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (n <= 0)
      return transport_error(error, "Could not write to provider");
    offset += n;
  }
  return TRUE;
}

static gboolean write_message(Transport *t, JsonNode *node, GError **error) {
  g_autofree char *json = json_to_string(node, FALSE);
  return write_frame(t, json, error);
}

static gboolean send_cancel(Transport *t) {
  g_autoptr(JsonObject) o = json_object_new();
  json_object_set_string_member(o, "thread_id", t->request->thread_id);
  json_object_set_string_member(o, "request_id", t->request->request_id);
  g_autoptr(JsonNode) p = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(p, o);
  g_autoptr(JsonNode) node =
      json_rpc_make_request("provider/cancel", g_steal_pointer(&p), 3);
  g_autofree char *json = json_to_string(node, FALSE);
  g_autofree char *line = g_strconcat(json, "\n", NULL);
  /* Small cancellation frame fits atomically into an available pipe; otherwise
   * the deadline below still terminates the isolated worker process group. */
  ssize_t n = write(t->input, line, strlen(line));
  return n == (ssize_t)strlen(line);
}

static JsonNode *read_message(Transport *t, gboolean submission,
                              GError **error) {
  for (;;) {
    if (g_cancellable_is_cancelled(t->runner->cancel) && !t->cancelling) {
      if (!submission) {
        transport_error(error, "Provider startup cancelled");
        return NULL;
      }
      t->cancelling = TRUE;
      t->deadline =
          g_get_monotonic_time() + PROMPTR_CANCEL_SECONDS * G_TIME_SPAN_SECOND;
      send_cancel(t);
    }
    if (t->deadline > 0 && g_get_monotonic_time() >= t->deadline) {
      transport_error(error, t->cancelling
                                 ? "Provider did not finish cancellation"
                                 : "Provider timed out");
      return NULL;
    }
    char *newline = memchr(t->buffer->str, '\n', t->buffer->len);
    if (newline != NULL) {
      gsize len = newline - t->buffer->str;
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, t->buffer->str, len, error))
        return NULL;
      JsonNode *root = json_parser_get_root(parser);
      if (!json_rpc_validate(root, error))
        return NULL;
      JsonNode *copy = json_node_copy(root);
      g_string_erase(t->buffer, 0, len + 1);
      return copy;
    }
    if (g_cancellable_is_cancelled(t->runner->cancel) && !t->cancelling) {
      if (!submission) {
        transport_error(error, "Provider startup cancelled");
        return NULL;
      }
      t->cancelling = TRUE;
      t->deadline =
          g_get_monotonic_time() + PROMPTR_CANCEL_SECONDS * G_TIME_SPAN_SECOND;
      send_cancel(t);
    }
    if (t->deadline > 0 && g_get_monotonic_time() >= t->deadline) {
      transport_error(error, t->cancelling
                                 ? "Provider did not finish cancellation"
                                 : "Provider timed out");
      return NULL;
    }
    struct pollfd fd = {t->output, POLLIN, 0};
    int ready = poll(&fd, 1, 50);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || (fd.revents & (POLLERR | POLLNVAL))) {
      transport_error(error, "Provider pipe failed");
      return NULL;
    }
    if (ready == 0)
      continue;
    char buf[16384];
    ssize_t n = read(t->output, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (n <= 0) {
      transport_error(error, "Provider exited before completing the request");
      return NULL;
    }
    if (memchr(buf, 0, n) != NULL || t->buffer->len + n > PROMPTR_MAX_FRAME) {
      transport_error(error, "Provider sent an invalid or oversized IPC frame");
      return NULL;
    }
    g_string_append_len(t->buffer, buf, n);
  }
}

static void child_setup(gpointer data) {
  (void)data;
  setsid();
#ifdef __linux__
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() == 1)
    _exit(1);
#endif
}

static void transport_close(Transport *t) {
  if (t->input >= 0)
    close(t->input);
  if (t->output >= 0)
    close(t->output);
  if (t->pid > 0) {
    int status;
    gint64 end =
        g_get_monotonic_time() + PROMPTR_SHUTDOWN_SECONDS * G_TIME_SPAN_SECOND;
    for (;;) {
      pid_t result = waitpid(t->pid, &status, WNOHANG);
      if (result == t->pid || (result < 0 && errno == ECHILD))
        break;
      if (g_get_monotonic_time() >= end) {
        kill(-t->pid, SIGKILL);
        while (waitpid(t->pid, &status, 0) < 0 && errno == EINTR) {
        }
        break;
      }
      poll(NULL, 0, 20);
    }
    /* Reap provider descendants as well if a broken plugin abandoned them. */
    kill(-t->pid, SIGTERM);
    g_spawn_close_pid(t->pid);
  }
  g_string_free(t->buffer, TRUE);
}

static void job_free(Job *j) {
  thread_store_unref(j->request.store);
  g_free(j->thread_id);
  g_free(j->request_id);
  g_free(j->query);
  g_free(j->model);
  g_free(j->agent_name);
  g_free(j->agent_content);
  g_free(j->project_dir);
  runner_unref(j->runner);
  g_free(j);
}

static void redact_error(JsonObject *settings, GError **error) {
  if (settings == NULL || error == NULL || *error == NULL)
    return;
  g_autoptr(GList) fields = json_object_get_members(settings);
  for (GList *it = fields; it != NULL; it = it->next) {
    const char *value = protocol_string(settings, it->data);
    if (value == NULL || !*value || strstr((*error)->message, value) == NULL)
      continue;
    g_auto(GStrv) parts = g_strsplit((*error)->message, value, -1);
    g_autofree char *safe = g_strjoinv("[redacted]", parts);
    g_free((*error)->message);
    (*error)->message = g_steal_pointer(&safe);
  }
}

static void run_job(Job *j) {
  Runner *r = j->runner;
  ProviderRequest *q = &j->request;
  g_autoptr(GError) error = NULL;
  g_autofree char *resolved_context =
      q->agent_content == NULL ? agent_context_resolve(q->agent_name, &error)
                               : NULL;
  if (resolved_context != NULL)
    q->agent_content = resolved_context;
  g_autoptr(JsonObject) settings =
      error == NULL ? provider_profile_resolve(r->profile, r->cancel, &error)
                    : NULL;
  Transport t = {.runner = r,
                 .input = -1,
                 .output = -1,
                 .buffer = g_string_new(NULL),
                 .request = q};
  gboolean began = FALSE, completed = FALSE, cancelled = FALSE,
           received = FALSE;
  guint64 output_end = 0;
  g_auto(GStrv) models = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  gint64 start = g_get_monotonic_time();
  if (settings == NULL)
    goto finished;
  argv = g_ptr_array_new();
  g_ptr_array_add(argv, r->profile->command);
  for (guint i = 0;
       r->profile->arguments != NULL && r->profile->arguments[i] != NULL; i++)
    g_ptr_array_add(argv, r->profile->arguments[i]);
  g_ptr_array_add(argv, NULL);
  if (!g_spawn_async_with_pipes(
          NULL, (char **)argv->pdata, NULL,
          G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
              G_SPAWN_STDERR_TO_DEV_NULL,
          child_setup, NULL, &t.pid, &t.input, &t.output, NULL, &error))
    goto finished;
  fcntl(t.input, F_SETFL, fcntl(t.input, F_GETFL) | O_NONBLOCK);
  fcntl(t.output, F_SETFL, fcntl(t.output, F_GETFL) | O_NONBLOCK);
  t.deadline =
      g_get_monotonic_time() + PROMPTR_STARTUP_SECONDS * G_TIME_SPAN_SECOND;
  {
    g_autoptr(JsonObject) init = json_object_new();
    json_object_set_string_member(init, "protocol_version",
                                  PROMPTR_PROTOCOL_VERSION);
    json_object_set_string_member(init, "purpose",
                                  q->discover_models ? "models" : "submit");
    json_object_set_object_member(init, "config", json_object_ref(settings));
    g_autoptr(JsonNode) params = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(params, init);
    g_autoptr(JsonNode) request = json_rpc_make_request(
        "provider/initialize", g_steal_pointer(&params), 1);
    if (!write_message(&t, request, &error))
      goto finished;
    g_autoptr(JsonNode) response = read_message(&t, FALSE, &error);
    if (response == NULL)
      goto finished;
    if (json_rpc_is_error(response)) {
      transport_error(&error, json_rpc_get_error_message(response));
      goto finished;
    }
    JsonNode *result = json_rpc_get_result(response);
    if (json_rpc_get_id(response) != 1 || result == NULL ||
        !JSON_NODE_HOLDS_OBJECT(result) ||
        g_strcmp0(
            protocol_string(json_node_get_object(result), "protocol_version"),
            PROMPTR_PROTOCOL_VERSION) != 0) {
      transport_error(&error, "Provider does not support protocol 2.0; install "
                              "a compatible plugin");
      goto finished;
    }
  }
  {
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "thread_id", q->thread_id);
    json_object_set_string_member(params, "request_id", q->request_id);
    const char *model =
        q->model != NULL && *q->model && g_strcmp0(q->model, "None") != 0
            ? q->model
            : r->profile->model;
    if (model != NULL && *model)
      json_object_set_string_member(params, "model", model);
    if (!q->discover_models) {
      g_autoptr(JsonArray) messages =
          q->store != NULL && q->follow_up
              ? thread_store_messages(q->store, &error)
              : json_array_new();
      if (messages == NULL)
        goto finished;
      JsonObject *user = json_object_new();
      json_object_set_string_member(user, "role", "user");
      json_object_set_string_member(user, "content", q->query);
      json_array_add_object_element(messages, user);
      json_object_set_array_member(params, "messages",
                                   json_array_ref(messages));
      if (q->agent_content != NULL) {
        JsonObject *agent = json_object_new();
        json_object_set_string_member(
            agent, "name", q->agent_name != NULL ? q->agent_name : "");
        json_object_set_string_member(agent, "content", q->agent_content);
        json_object_set_object_member(params, "agent", agent);
      }
      if (q->project_dir != NULL && *q->project_dir)
        json_object_set_string_member(params, "project_dir", q->project_dir);
      json_object_set_boolean_member(params, "connection_test",
                                     q->connection_test);
      if (q->store != NULL && q->follow_up) {
        g_autofree char *state =
            thread_store_get_resume(q->store, r->profile->id);
        if (state != NULL)
          json_object_set_string_member(params, "resume_state", state);
      }
    }
    g_autoptr(JsonNode) p = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(p, params);
    g_autoptr(JsonNode) request = json_rpc_make_request(
        q->discover_models ? "provider/models" : "provider/submit",
        g_steal_pointer(&p), 2);
    t.deadline =
        g_get_monotonic_time() + (q->connection_test || q->discover_models
                                      ? PROMPTR_TEST_SECONDS
                                      : PROMPTR_STARTUP_SECONDS) *
                                     G_TIME_SPAN_SECOND;
    g_autofree char *encoded = json_to_string(request, FALSE);
    if (strlen(encoded) > PROMPTR_MAX_FRAME) {
      transport_error(&error, "Request exceeds the 64 MiB IPC frame limit; "
                              "start a fresh conversation");
      goto finished;
    }
    if (q->store != NULL && !q->discover_models) {
      if (!thread_store_begin(q->store, q->request_id, q->query, !q->follow_up,
                              &error))
        goto finished;
      began = TRUE;
    }
    if (!write_frame(&t, encoded, &error))
      goto finished;
    if (!q->connection_test && !q->discover_models)
      t.deadline = 0;
  }
  /* The provider now owns its input snapshot. Keep only routing, persistence
   * and cancellation state while awaiting output, including for background
   * tabs. */
  g_clear_pointer(&j->query, g_free);
  q->query = NULL;
  g_clear_pointer(&j->agent_content, g_free);
  q->agent_content = NULL;
  g_clear_pointer(&resolved_context, g_free);
  while (!completed) {
    g_autoptr(JsonNode) response = read_message(&t, TRUE, &error);
    if (response == NULL)
      break;
    JsonObject *o = json_node_get_object(response);
    if (g_strcmp0(protocol_string(o, "method"), "provider/event") == 0) {
      JsonObject *p = protocol_object(o, "params");
      if (g_strcmp0(protocol_string(p, "thread_id"), q->thread_id) != 0 ||
          g_strcmp0(protocol_string(p, "request_id"), q->request_id) != 0)
        continue;
      const char *type = protocol_string(p, "type");
      if (g_strcmp0(type, "progress") == 0)
        continue;
      const char *text = protocol_string(p, "output");
      if (g_strcmp0(type, "delta") != 0 || text == NULL ||
          strlen(text) > PROMPTR_MAX_DELTA) {
        transport_error(
            &error, "Provider sent an unsupported event or invalid text delta");
        break;
      }
      output_end += strlen(text);
      if (began && !thread_store_chunk(q->store, text, &error)) {
        if (!q->connection_test && *text)
          post_event(r, PROVIDER_EVENT_CHUNK, text, NULL, 0, NULL, output_end);
        break;
      }
      if (*text)
        received = TRUE;
      if (!q->connection_test && *text)
        post_event(r, PROVIDER_EVENT_CHUNK, text, NULL, 0, NULL, output_end);
      continue;
    }
    if (json_rpc_get_id(response) == 3 && t.cancelling &&
        json_rpc_is_response(response))
      continue;
    if (json_rpc_get_id(response) != 2) {
      transport_error(&error, "Unexpected provider response ID");
      break;
    }
    if (json_rpc_is_error(response)) {
      transport_error(&error, json_rpc_get_error_message(response));
      break;
    }
    JsonObject *result = protocol_object(o, "result");
    if (q->discover_models) {
      JsonNode *list =
          result != NULL ? json_object_get_member(result, "models") : NULL;
      if (list == NULL || !JSON_NODE_HOLDS_ARRAY(list)) {
        transport_error(&error, "Provider does not support model discovery");
        break;
      }
      JsonArray *a = json_node_get_array(list);
      if (json_array_get_length(a) > 10000) {
        transport_error(&error, "Provider returned more than 10000 models; "
                                "enter a model ID manually");
        break;
      }
      models = g_new0(char *, json_array_get_length(a) + 1);
      for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonNode *v = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_VALUE(v) ||
            json_node_get_value_type(v) != G_TYPE_STRING ||
            strlen(json_node_get_string(v)) > 1024) {
          transport_error(&error, "Provider sent an invalid model list");
          break;
        }
        models[i] = g_strdup(json_node_get_string(v));
      }
      completed = error == NULL;
      break;
    }
    if (g_strcmp0(protocol_string(result, "thread_id"), q->thread_id) != 0 ||
        g_strcmp0(protocol_string(result, "request_id"), q->request_id) != 0) {
      transport_error(&error,
                      "Provider completion IDs do not match the request");
      break;
    }
    const char *status = protocol_string(result, "status");
    cancelled = g_strcmp0(status, "cancelled") == 0;
    completed = cancelled || g_strcmp0(status, "completed") == 0;
    if (!completed) {
      transport_error(&error, "Provider sent an invalid completion status");
      break;
    }
    if (!received && !cancelled) {
      transport_error(&error, "Provider completed without receiving an answer");
      completed = FALSE;
      break;
    }
    if (began && !thread_store_set_resume(
                     q->store, r->profile->id,
                     protocol_string(result, "resume_state"), &error))
      break;
  }
finished:
  cancelled = cancelled || g_cancellable_is_cancelled(r->cancel);
  if (error != NULL && t.pid > 0)
    kill(-t.pid, SIGTERM);
  transport_close(&t);
  if (began) {
    g_autoptr(GError) save_error = NULL;
    if (!thread_store_finish(q->store,
                             cancelled                    ? "cancelled"
                             : completed && error == NULL ? "completed"
                                                          : "failed",
                             &save_error)) {
      g_clear_error(&error);
      error = g_steal_pointer(&save_error);
      cancelled = FALSE;
    }
  }
  redact_error(settings, &error);
  post_event(r,
             error == NULL && completed && !cancelled ? PROVIDER_EVENT_DONE
                                                      : PROVIDER_EVENT_ERROR,
             NULL,
             cancelled       ? ""
             : error != NULL ? error->message
                             : "Provider request failed",
             g_get_monotonic_time() - start, models, output_end);
}

static gpointer request_worker(gpointer data) {
  Job *job = data;
  run_job(job);
  job_free(job);
  return NULL;
}

static gboolean runner_submit(void *impl, gpointer source,
                              const ProviderRequest *q, GCancellable *cancel,
                              ProviderCallback callback, gpointer callback_data,
                              GError **error) {
  Runner *r = impl;
  if (r->busy)
    return transport_error(error, "Provider already has a pending request");
  if (q == NULL || q->thread_id == NULL || q->request_id == NULL ||
      q->query == NULL)
    return transport_error(error, "Invalid provider request");
  r->busy = TRUE;
  r->source = source;
  r->callback = callback;
  r->callback_data = callback_data;
  g_clear_object(&r->cancel);
  r->cancel = cancel != NULL ? g_object_ref(cancel) : g_cancellable_new();
  Job *j = g_new0(Job, 1);
  j->runner = runner_ref(r);
  j->request = *q;
  j->thread_id = g_strdup(q->thread_id);
  j->request.thread_id = j->thread_id;
  j->request_id = g_strdup(q->request_id);
  j->request.request_id = j->request_id;
  j->query = g_strdup(q->query);
  j->request.query = j->query;
  j->model = g_strdup(q->model);
  j->request.model = j->model;
  j->agent_name = g_strdup(q->agent_name);
  j->request.agent_name = j->agent_name;
  j->agent_content = g_strdup(q->agent_content);
  j->request.agent_content = j->agent_content;
  j->project_dir = g_strdup(q->project_dir);
  j->request.project_dir = j->project_dir;
  j->request.store = thread_store_ref(q->store);
  GThread *thread =
      g_thread_try_new("provider-request", request_worker, j, error);
  if (thread == NULL) {
    r->busy = FALSE;
    job_free(j);
    return FALSE;
  }
  g_thread_unref(thread);
  return TRUE;
}

static void runner_cancel(void *impl) {
  Runner *r = impl;
  if (r->cancel != NULL)
    g_cancellable_cancel(r->cancel);
  g_mutex_lock(&r->mutex);
  g_cond_broadcast(&r->drained);
  g_mutex_unlock(&r->mutex);
}
static void runner_cancel_tab(void *impl, gpointer source) {
  (void)source;
  runner_cancel(impl);
}
static void runner_destroy(void *impl) {
  Runner *r = impl;
  runner_cancel(r);
  runner_unref(r);
}
static char *display_string(void *impl, const char *query, const char *model,
                            const char *agent, gboolean follow_up) {
  Runner *r = impl;
  (void)query;
  (void)agent;
  (void)follow_up;
  return g_strdup_printf("%s%s%s", r->profile->name, model != NULL ? " / " : "",
                         model != NULL ? model : "");
}
static ProviderVTable vtable = {.name = "plugin",
                                .destroy = runner_destroy,
                                .submit = runner_submit,
                                .cancel = runner_cancel,
                                .cancel_tab = runner_cancel_tab,
                                .get_display_string = display_string};

Provider *plugin_runner_create_profile(const ProviderProfile *profile) {
  Runner *r = g_new0(Runner, 1);
  r->refs = 1;
  r->profile = provider_profile_copy(profile);
  g_mutex_init(&r->mutex);
  g_cond_init(&r->drained);
  Provider *p = g_new0(Provider, 1);
  p->vtable = &vtable;
  p->impl = r;
  return p;
}
