#include "../../lib/promptr-protocol/provider-server.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static char *read_bounded_event(GDataInputStream *stream, gsize *length,
                                GCancellable *cancel, GError **error) {
  g_autoptr(GString) line = g_string_new(NULL);
  for (;;) {
    guint8 ch;
    gssize count =
        g_input_stream_read(G_INPUT_STREAM(stream), &ch, 1, cancel, error);
    if (count < 0)
      return NULL;
    if (count == 0 && line->len == 0)
      return NULL;
    if (count == 0 || ch == '\n') {
      *length = line->len;
      return g_string_free(g_steal_pointer(&line), FALSE);
    }
    if (ch == 0 || line->len >= PROMPTR_MAX_FRAME) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "OpenCode event exceeds limit or contains NUL");
      return NULL;
    }
    g_string_append_c(line, ch);
  }
}

#ifndef PROMPTR_OPENCODE_TEST_SECONDS
#define PROMPTR_OPENCODE_TEST_SECONDS 50
#endif

typedef struct {
  GMutex mutex;
  GCond changed;
  GThread *thread;
  GSubprocess *process;
  const char *diagnostic_path;
  char *fatal_message;
  gboolean done, timed_out;
  guint timeout_seconds;
} BackendMonitor;

static char *permanent_backend_error(const char *path) {
  FILE *file = g_fopen(path, "rb");
  if (file == NULL)
    return NULL;
  char tail[4097];
  gsize length = 0;
  if (fseek(file, 0, SEEK_END) == 0) {
    long size = ftell(file);
    if (size > 0 && fseek(file, MAX(0L, size - 4096), SEEK_SET) == 0)
      length = fread(tail, 1, sizeof(tail) - 1, file);
  }
  fclose(file);
  tail[length] = '\0';
  if (strstr(tail, "level=ERROR") == NULL)
    return NULL;
  g_autofree char *valid = g_utf8_make_valid(tail, length);
  g_autoptr(GRegex) field =
      g_regex_new("error\\.error=\"((?:\\\\.|[^\"\\\\])*)\"", 0, 0, NULL);
  g_autoptr(GRegex) permanent = g_regex_new(
      "(?i)(free usage limit reached|insufficient[_ ]quota|quota "
      "(?:exceeded|exhausted)|invalid api key|unauthorized|authentication "
      "failed|not authenticated|model.{0,40}not found|HTTP[_ "
      "]?(?:401|402|403|404))",
      0, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  g_regex_match(field, valid, 0, &match);
  while (g_match_info_matches(match)) {
    g_autofree char *value = g_match_info_fetch(match, 1);
    gint offset = 0;
    g_match_info_fetch_pos(match, 0, &offset, NULL);
    const char *line = valid + offset;
    while (line > valid && line[-1] != '\n')
      line--;
    const char *end = strchr(line, '\n');
    gsize line_length = end != NULL ? (gsize)(end - line) : strlen(line);
    if (g_strstr_len(line, line_length, "level=ERROR") != NULL &&
        g_strstr_len(line, line_length, "small=true") == NULL &&
        g_regex_match(permanent, value, 0, NULL)) {
      g_autofree char *quoted = g_strdup_printf("\"%s\"", value);
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, quoted, -1, NULL))
        return g_strdup(json_node_get_string(json_parser_get_root(parser)));
      return g_strdup(value);
    }
    if (!g_match_info_next(match, NULL))
      break;
  }
  return NULL;
}

static gpointer backend_monitor_worker(gpointer data) {
  BackendMonitor *deadline = data;
  gint64 until = deadline->timeout_seconds != 0
                     ? g_get_monotonic_time() +
                           deadline->timeout_seconds * G_TIME_SPAN_SECOND
                     : G_MAXINT64;
  g_mutex_lock(&deadline->mutex);
  while (!deadline->done) {
    g_cond_wait_until(&deadline->changed, &deadline->mutex,
                      MIN(until, g_get_monotonic_time() + 200 * 1000));
    if (deadline->done)
      break;
    g_mutex_unlock(&deadline->mutex);
    char *fatal = deadline->diagnostic_path != NULL
                      ? permanent_backend_error(deadline->diagnostic_path)
                      : NULL;
    g_mutex_lock(&deadline->mutex);
    if (deadline->done) {
      g_free(fatal);
      break;
    }
    if (fatal != NULL || g_get_monotonic_time() >= until) {
      deadline->fatal_message = fatal;
      deadline->timed_out = fatal == NULL;
      g_subprocess_force_exit(deadline->process);
      break;
    }
  }
  g_mutex_unlock(&deadline->mutex);
  return NULL;
}

static gboolean stop_backend_monitor(BackendMonitor *deadline) {
  if (deadline->thread == NULL)
    return FALSE;
  g_mutex_lock(&deadline->mutex);
  deadline->done = TRUE;
  g_cond_signal(&deadline->changed);
  g_mutex_unlock(&deadline->mutex);
  g_thread_join(deadline->thread);
  deadline->thread = NULL;
  g_mutex_clear(&deadline->mutex);
  g_cond_clear(&deadline->changed);
  return deadline->timed_out;
}

static char *safe_diagnostic(const char *text) {
  g_autoptr(GRegex) credentials =
      g_regex_new("(?i)(\\b(?:authorization|api[_-]?key|access[_-]?token|"
                  "password)[\\\"' :=]+(?:Bearer[ ]+)?)[^\\s,}\\\"']+",
                  0, 0, NULL);
  char *safe =
      g_regex_replace(credentials, text, -1, 0, "\\1[redacted]", 0, NULL);
  g_auto(GStrv) variables = g_listenv();
  for (guint i = 0; variables != NULL && variables[i] != NULL; i++) {
    const char *name = variables[i];
    if (strstr(name, "KEY") == NULL && strstr(name, "TOKEN") == NULL &&
        strstr(name, "SECRET") == NULL && strstr(name, "PASSWORD") == NULL)
      continue;
    const char *value = g_getenv(name);
    if (value == NULL || strlen(value) < 8 || strstr(safe, value) == NULL)
      continue;
    g_auto(GStrv) pieces = g_strsplit(safe, value, -1);
    g_free(safe);
    safe = g_strjoinv("[redacted]", pieces);
  }
  return safe;
}

static const char *binary(JsonObject *settings) {
  const char *bin = protocol_string(settings, "opencode_path");
  return bin != NULL && *bin ? bin : "opencode";
}

static gboolean initialize(JsonObject *settings, gboolean discovery,
                           GError **error) {
  (void)discovery;
  g_autofree char *path = g_find_program_in_path(binary(settings));
  if (path == NULL) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "OpenCode executable was not found; configure its absolute path");
    return FALSE;
  }
  return TRUE;
}

static void cancelled_child(GCancellable *cancel, gpointer data) {
  (void)cancel;
  g_subprocess_force_exit(G_SUBPROCESS(data));
}

static void backend_child_setup(gpointer data) {
  (void)data;
#ifdef __linux__
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() == 1)
    _exit(1);
#endif
}

static gboolean private_server_supported(JsonObject *settings,
                                         GCancellable *cancel,
                                         gboolean *supported, GError **error) {
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_child_setup(launcher, backend_child_setup, NULL,
                                        NULL);
  g_autoptr(GSubprocess) process = g_subprocess_launcher_spawn(
      launcher, error, binary(settings), "--version", NULL);
  if (process == NULL)
    return FALSE;
  BackendMonitor monitor = {.process = process, .timeout_seconds = 5};
  g_mutex_init(&monitor.mutex);
  g_cond_init(&monitor.changed);
  monitor.thread =
      g_thread_new("opencode-version", backend_monitor_worker, &monitor);
  gulong handler = g_cancellable_connect(cancel, G_CALLBACK(cancelled_child),
                                         g_object_ref(process), g_object_unref);
  char output[128] = {0};
  gsize length = 0;
  gboolean ok =
      g_input_stream_read_all(g_subprocess_get_stdout_pipe(process), output,
                              sizeof(output) - 1, &length, cancel, error);
  if (!ok || length == sizeof(output) - 1)
    g_subprocess_force_exit(process);
  if (!g_subprocess_wait_check(process, cancel, ok ? error : NULL))
    ok = FALSE;
  if (stop_backend_monitor(&monitor)) {
    g_clear_error(error);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "OpenCode version check timed out");
    ok = FALSE;
  }
  g_cancellable_disconnect(cancel, handler);
  if (!ok)
    return FALSE;
  const char *version = g_str_has_prefix(output, "opencode v")
                            ? output + strlen("opencode v")
                            : output;
  char *end = NULL;
  guint64 major = g_ascii_strtoull(version, &end, 10);
  if (!g_ascii_isdigit(*version) || end == NULL || *end != '.' ||
      length == sizeof(output) - 1) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "OpenCode returned an unrecognized version");
    return FALSE;
  }
  *supported = major >= 2;
  return TRUE;
}

static gboolean submit(JsonObject *settings, JsonObject *request,
                       GCancellable *cancel, ProviderEmit emit,
                       gpointer emit_data, char **resume, GError **error) {
  gboolean standalone = FALSE;
  if (!private_server_supported(settings, cancel, &standalone, error))
    return FALSE;
  JsonArray *messages = json_object_get_array_member(request, "messages");
  guint count = json_array_get_length(messages);
  JsonObject *agent = protocol_object(request, "agent");
  const char *context = protocol_string(agent, "content");
  const char *model = protocol_string(request, "model");
  const char *project = protocol_string(request, "project_dir");
  gboolean connection_test = protocol_boolean(request, "connection_test");
  if (connection_test &&
      (model == NULL || !*model || g_str_equal(model, "None")))
    model = "opencode/big-pickle";
  g_autofree char *agent_hash = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, context != NULL ? context : "", -1);
  g_autofree char *history_hash = provider_history_digest(messages, count - 1);
  g_autoptr(JsonParser) state_parser = json_parser_new();
  const char *old_state = protocol_string(request, "resume_state");
  JsonObject *state = NULL;
  if (old_state != NULL &&
      json_parser_load_from_data(state_parser, old_state, -1, NULL) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(state_parser)))
    state = json_node_get_object(json_parser_get_root(state_parser));
  gboolean reuse =
      g_strcmp0(protocol_string(state, "history_hash"), history_hash) == 0 &&
      g_strcmp0(protocol_string(state, "agent_hash"), agent_hash) == 0 &&
      g_strcmp0(protocol_string(state, "model"), model != NULL ? model : "") ==
          0 &&
      g_strcmp0(protocol_string(state, "project"),
                project != NULL ? project : "") == 0 &&
      protocol_string(state, "session_id") != NULL;
  g_autoptr(GString) prompt = g_string_new(NULL);
  g_autofree char *scratch = g_dir_make_tmp("promptr-opencode-XXXXXX", error);
  if (scratch == NULL)
    return FALSE;
  g_autofree char *attachment =
      g_build_filename(scratch, "conversation.txt", NULL);
  g_autofree char *context_path = g_build_filename(scratch, "agent.md", NULL);
  g_autofree char *stderr_path = g_build_filename(scratch, "stderr.txt", NULL);
  gboolean ok = TRUE;
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  gsize answer_bytes = 0;
  g_autofree char *workdir = NULL;
  g_autofree char *settings_json = NULL;
  g_autofree char *settings_hash = NULL;
  {
    g_autoptr(JsonNode) settings_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(settings_node, settings);
    settings_json = json_to_string(settings_node, FALSE);
    settings_hash =
        g_compute_checksum_for_string(G_CHECKSUM_SHA256, settings_json, -1);
  }
  reuse = reuse && g_strcmp0(protocol_string(state, "settings_hash"),
                             settings_hash) == 0;
  if (project == NULL || !*project) {
    g_autofree char *key = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, protocol_string(request, "thread_id"), -1);
    workdir = g_build_filename(g_get_user_cache_dir(), "promptr-opencode",
                               "workspaces", key, NULL);
    if (g_mkdir_with_parents(workdir, 0700) != 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Could not create OpenCode workspace");
      ok = FALSE;
    }
  }
  /* The backend may reject an expired opaque session. A fresh retry is safe
   * only before it has emitted any answer text. */
  gboolean retried = FALSE;
  g_autofree char *session_id = NULL;
  gulong cancel_handler = 0;
  BackendMonitor deadline = {0};
  if (ok && context != NULL &&
      !g_file_set_contents_full(context_path, context, -1,
                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
    ok = FALSE;
  if (!ok)
    goto cleanup;
launch:
  g_string_truncate(prompt, 0);
  g_checksum_reset(checksum);
  for (guint i = 0; i < count; i++) {
    JsonObject *m = json_array_get_object_element(messages, i);
    provider_digest_message(checksum, protocol_string(m, "role"),
                            protocol_string(m, "content"));
    if (!reuse || i == count - 1)
      g_string_append_printf(prompt, "%s:\n%s\n\n", protocol_string(m, "role"),
                             protocol_string(m, "content"));
  }
  const char assistant_role[] = "assistant";
  g_checksum_update(checksum, (const guchar *)assistant_role,
                    sizeof(assistant_role));
  if (!g_file_set_contents_full(attachment, prompt->str, prompt->len,
                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
    ok = FALSE;
    goto cleanup;
  }
  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_set_stderr_file_path(launcher, stderr_path);
  g_subprocess_launcher_set_child_setup(launcher, backend_child_setup, NULL,
                                        NULL);
  if (project != NULL && *project)
    g_subprocess_launcher_set_cwd(launcher, project);
  else
    g_subprocess_launcher_set_cwd(launcher, workdir);
  {
    g_autoptr(JsonObject) config = json_object_new();
    JsonObject *agents = json_object_new(), *custom = json_object_new();
    json_object_set_string_member(custom, "description",
                                  "Promptr conversation context");
    json_object_set_string_member(custom, "mode", "primary");
    g_autofree char *context_reference =
        context != NULL ? g_strdup_printf("{file:%s}", context_path)
                        : g_strdup("Respond to the user's question.");
    json_object_set_string_member(custom, "prompt", context_reference);
    json_object_set_object_member(agents, "promptr-context", custom);
    json_object_set_object_member(config, "agent", agents);
    if (connection_test) {
      JsonObject *permissions = json_object_new();
      json_object_set_string_member(permissions, "*", "deny");
      json_object_set_object_member(config, "permission", permissions);
    }
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, config);
    g_autofree char *text = json_to_string(node, FALSE);
    g_subprocess_launcher_setenv(launcher, "OPENCODE_CONFIG_CONTENT", text,
                                 TRUE);
  }
  args = g_ptr_array_new();
  g_ptr_array_add(args, (gpointer)binary(settings));
  g_ptr_array_add(args, "run");
  /* OpenCode 2's shared service does not inherit this request's agent config.
   */
  if (standalone)
    g_ptr_array_add(args, "--standalone");
  g_ptr_array_add(args, "--print-logs");
  g_ptr_array_add(args, "--log-level");
  g_ptr_array_add(args, "warn");
  g_ptr_array_add(args, "--format");
  g_ptr_array_add(args, "json");
  g_ptr_array_add(args, "--agent");
  g_ptr_array_add(args, "promptr-context");
  if (model != NULL && *model) {
    g_ptr_array_add(args, "--model");
    g_ptr_array_add(args, (gpointer)model);
  }
  if (reuse) {
    g_ptr_array_add(args, "--session");
    g_ptr_array_add(args, (gpointer)protocol_string(state, "session_id"));
  }
  if (!connection_test) {
    g_ptr_array_add(args, "--file");
    g_ptr_array_add(args, attachment);
  }
  g_ptr_array_add(args, "--");
  g_ptr_array_add(
      args,
      connection_test
          ? (gpointer)protocol_string(
                json_array_get_object_element(messages, count - 1), "content")
          : "Respond to the last user message in the attached conversation.");
  g_ptr_array_add(args, NULL);
  process = g_subprocess_launcher_spawnv(
      launcher, (const char *const *)args->pdata, error);
  if (process == NULL) {
    ok = FALSE;
    goto cleanup;
  }
  g_mutex_init(&deadline.mutex);
  g_cond_init(&deadline.changed);
  deadline.done = deadline.timed_out = FALSE;
  deadline.timeout_seconds =
      connection_test ? PROMPTR_OPENCODE_TEST_SECONDS : 0;
  deadline.process = process;
  deadline.diagnostic_path = stderr_path;
  deadline.thread =
      g_thread_new("opencode-monitor", backend_monitor_worker, &deadline);
  cancel_handler = g_cancellable_connect(cancel, G_CALLBACK(cancelled_child),
                                         g_object_ref(process), g_object_unref);
  {
    g_autoptr(GDataInputStream) stream =
        g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    for (;;) {
      gsize len;
      g_autofree char *line = read_bounded_event(stream, &len, cancel, error);
      if (line == NULL) {
        if (error != NULL && *error != NULL)
          ok = FALSE;
        break;
      }
      if (len > PROMPTR_MAX_FRAME) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "OpenCode event exceeds IPC limit");
        ok = FALSE;
        break;
      }
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, line, len, error) ||
          !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        if (error != NULL && *error == NULL)
          g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "OpenCode emitted an invalid JSON event");
        ok = FALSE;
        break;
      }
      JsonObject *o = json_node_get_object(json_parser_get_root(parser));
      const char *sid = protocol_string(o, "sessionID");
      if (sid != NULL) {
        g_free(session_id);
        session_id = g_strdup(sid);
      }
      if (g_strcmp0(protocol_string(o, "type"), "error") == 0) {
        JsonObject *detail = protocol_object(o, "error");
        const char *message =
            protocol_string(protocol_object(detail, "data"), "message");
        if (message == NULL)
          message = protocol_string(detail, "message");
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            message != NULL
                                ? message
                                : "OpenCode reported an error; check its "
                                  "login, model and version");
        ok = FALSE;
        break;
      }
      if (g_strcmp0(protocol_string(o, "type"), "text") != 0)
        continue;
      const char *text = protocol_string(protocol_object(o, "part"), "text");
      if (text != NULL && *text) {
        if (!emit(text, emit_data, error)) {
          ok = FALSE;
          break;
        }
        g_checksum_update(checksum, (const guchar *)text, strlen(text));
        answer_bytes += strlen(text);
      }
    }
  }
  if (!ok)
    g_subprocess_force_exit(process);
  if (!g_subprocess_wait_check(process, cancel, ok ? error : NULL))
    ok = FALSE;
  if (stop_backend_monitor(&deadline)) {
    g_clear_error(error);
    g_set_error(
        error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
        "OpenCode did not answer the connection test within %d seconds. "
        "Check access to the selected model or try a different model.",
        PROMPTR_OPENCODE_TEST_SECONDS);
    ok = FALSE;
  }
  if (deadline.fatal_message == NULL && !g_cancellable_is_cancelled(cancel))
    deadline.fatal_message = permanent_backend_error(stderr_path);
  if (deadline.fatal_message != NULL) {
    g_clear_error(error);
    const char *message = deadline.fatal_message;
    if (g_str_has_prefix(message, "AI_APICallError: "))
      message += strlen("AI_APICallError: ");
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OpenCode: %s", message);
    ok = FALSE;
  }
  if (ok && answer_bytes == 0 && !g_cancellable_is_cancelled(cancel)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "OpenCode exited without an answer. Check its login "
                        "and selected model.");
    ok = FALSE;
  }
  g_cancellable_disconnect(cancel, cancel_handler);
  cancel_handler = 0;
  if (!ok && reuse && !retried && answer_bytes == 0 &&
      deadline.fatal_message == NULL && !g_cancellable_is_cancelled(cancel)) {
    retried = TRUE;
    reuse = FALSE;
    ok = TRUE;
    g_clear_error(error);
    g_clear_object(&process);
    g_clear_object(&launcher);
    g_clear_pointer(&args, g_ptr_array_unref);
    g_clear_pointer(&session_id, g_free);
    goto launch;
  }
  if (ok && session_id != NULL &&
      !protocol_boolean(request, "connection_test")) {
    const guchar terminator = 0;
    g_checksum_update(checksum, &terminator, 1);
    g_autoptr(JsonObject) out = json_object_new();
    json_object_set_string_member(out, "session_id", session_id);
    json_object_set_string_member(out, "history_hash",
                                  g_checksum_get_string(checksum));
    json_object_set_string_member(out, "agent_hash", agent_hash);
    json_object_set_string_member(out, "settings_hash", settings_hash);
    json_object_set_string_member(out, "model", model != NULL ? model : "");
    json_object_set_string_member(out, "project",
                                  project != NULL ? project : "");
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, out);
    *resume = json_to_string(node, FALSE);
  }
cleanup:
  stop_backend_monitor(&deadline);
  g_clear_pointer(&deadline.fatal_message, g_free);
  if (cancel_handler != 0)
    g_cancellable_disconnect(cancel, cancel_handler);
  g_unlink(attachment);
  g_unlink(context_path);
  if (!ok && error != NULL && *error != NULL) {
    FILE *file = g_fopen(stderr_path, "rb");
    if (file != NULL) {
      if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && fseek(file, MAX(0L, size - 2048), SEEK_SET) == 0) {
          char tail[2049];
          gsize length = fread(tail, 1, sizeof(tail) - 1, file);
          tail[length] = '\0';
          g_autofree char *valid = g_utf8_make_valid(tail, length);
          g_autoptr(GRegex) ansi =
              g_regex_new("\\x1b\\[[0-?]*[ -/]*[@-~]", 0, 0, NULL);
          g_autofree char *plain =
              g_regex_replace(ansi, valid, -1, 0, "", 0, NULL);
          g_autoptr(GString) message = g_string_new((*error)->message);
          g_string_append_printf(message, "\n\nOpenCode details:\n%s",
                                 g_strstrip(plain != NULL ? plain : valid));
          g_free((*error)->message);
          (*error)->message = g_string_free(g_steal_pointer(&message), FALSE);
        }
      }
      fclose(file);
    }
  }
  g_unlink(stderr_path);
  if (!ok && error != NULL && *error != NULL) {
    char *safe = safe_diagnostic((*error)->message);
    g_free((*error)->message);
    (*error)->message = safe;
  }
  g_rmdir(scratch);
  return ok;
}

static char **models(JsonObject *settings, GCancellable *cancel,
                     GError **error) {
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error,
      binary(settings), "models", NULL);
  if (process == NULL)
    return NULL;
  gulong handler = g_cancellable_connect(cancel, G_CALLBACK(cancelled_child),
                                         g_object_ref(process), g_object_unref);
  char *output = NULL;
  gboolean ok = g_subprocess_communicate_utf8(process, NULL, cancel, &output,
                                              NULL, error) &&
                g_subprocess_get_successful(process);
  g_cancellable_disconnect(cancel, handler);
  if (!ok) {
    g_free(output);
    if (error != NULL && *error == NULL)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "OpenCode model discovery failed");
    return NULL;
  }
  char **list = g_strsplit(output != NULL ? g_strstrip(output) : "", "\n", -1);
  g_free(output);
  return list;
}

int main(void) {
  const ProviderBackend backend = {initialize, submit, models};
  return provider_server_run(&backend);
}
