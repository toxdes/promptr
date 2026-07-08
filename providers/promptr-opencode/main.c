#include "../../lib/promptr-protocol/jsonrpc.h"
#include <errno.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── config default ────────────────────────────────────────────── */

#define DEFAULT_OPENCODE_PATH "opencode"
#define TMPDIR_PREFIX         "/tmp/promptr-opencode"

/* ── per-tab session state ─────────────────────────────────────── */

typedef struct {
  gboolean has_session;
  char *session_id;
  char *tmpdir_path;
} TabSession;

typedef struct {
  char *opencode_bin;
  GHashTable *sessions; /* char* tab_id -> TabSession* */

  /* Current submit state (one at a time) */
  char *active_tab_id;
  GPid child_pid;
  int child_stdout_fd;
  int child_stdin_fd;
  int child_stderr_fd;
  guint child_stdout_watch;
  GString *child_output_buf;
  gint64 submit_start;
} ProviderState;

/* ── forward declarations ──────────────────────────────────────── */

static void session_free(gpointer data);
static void cancel_current(ProviderState *state);
static gboolean handle_initialize(ProviderState *state, JsonNode *params);
static gboolean handle_submit(ProviderState *state, JsonNode *params);
static gboolean handle_cancel(ProviderState *state);
static gboolean handle_cleanup(ProviderState *state, const char *tab_id);

/* ── helpers ───────────────────────────────────────────────────── */

static void write_event(const char *tab_id, const char *type,
                        const char *output) {
  g_autoptr(JsonBuilder) params = json_builder_new();
  g_autoptr(JsonNode) event = NULL;

  json_builder_begin_object(params);
  json_builder_set_member_name(params, "tab_id");
  json_builder_add_string_value(params, tab_id != NULL ? tab_id : "");
  json_builder_set_member_name(params, "type");
  json_builder_add_string_value(params, type);
  json_builder_set_member_name(params, "output");
  json_builder_add_string_value(params, output != NULL ? output : "");
  json_builder_end_object(params);

  {
    JsonNode *p = json_builder_get_root(params);
    event = json_rpc_make_notification("provider/event", p);
  }

  json_rpc_write(stdout, event, NULL);
  fflush(stdout);
}

static void write_error_event(const char *tab_id, const char *msg) {
  g_autoptr(JsonBuilder) params = json_builder_new();
  g_autoptr(JsonNode) event = NULL;

  json_builder_begin_object(params);
  json_builder_set_member_name(params, "tab_id");
  json_builder_add_string_value(params, tab_id != NULL ? tab_id : "");
  json_builder_set_member_name(params, "type");
  json_builder_add_string_value(params, "error");
  json_builder_set_member_name(params, "error_msg");
  json_builder_add_string_value(params, msg != NULL ? msg : "");
  json_builder_end_object(params);

  {
    JsonNode *p = json_builder_get_root(params);
    event = json_rpc_make_notification("provider/event", p);
  }

  json_rpc_write(stdout, event, NULL);
  fflush(stdout);
}

/* ── NDJSON parser (extracted from opencode.c) ────────────────── */

static char *parse_ndjson(const char *output, char **out_session_id) {
  g_autofree char *session_id = NULL;
  GString *text;
  g_autofree char **lines = NULL;

  if (output == NULL || output[0] == '\0') {
    if (out_session_id != NULL)
      *out_session_id = NULL;
    return g_strdup("");
  }

  text = g_string_new(NULL);
  lines = g_strsplit(output, "\n", -1);

  for (int i = 0; lines[i] != NULL; i++) {
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    JsonNode *root;
    JsonObject *obj;
    const char *type, *sid;

    if (lines[i][0] == '\0')
      continue;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, lines[i], -1, &error))
      continue;

    root = json_parser_get_root(parser);
    if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
      continue;

    obj = json_node_get_object(root);

    if (session_id == NULL && json_object_has_member(obj, "sessionID")) {
      sid = json_object_get_string_member(obj, "sessionID");
      if (sid != NULL && sid[0] != '\0')
        session_id = g_strdup(sid);
    }

    type = json_object_get_string_member(obj, "type");
    if (type == NULL)
      continue;

    if (strcmp(type, "text") == 0) {
      JsonObject *part = json_object_get_object_member(obj, "part");

      if (part != NULL) {
        const char *content = json_object_get_string_member(part, "text");

        if (content != NULL)
          g_string_append(text, content);
      }
    }
  }

  if (out_session_id != NULL)
    *out_session_id = g_strdup(session_id);

  return g_string_free(text, FALSE);
}

/* ── session management ────────────────────────────────────────── */

static void session_free(gpointer data) {
  TabSession *sess = data;

  if (sess == NULL)
    return;
  g_free(sess->session_id);
  g_free(sess->tmpdir_path);
  g_free(sess);
}

static void cleanup_tab_session(ProviderState *state, const char *tab_id) {
  TabSession *sess = g_hash_table_lookup(state->sessions, tab_id);

  if (sess == NULL)
    return;

  if (sess->session_id != NULL) {
    g_spawn_async(
        NULL,
        (char *[]){"opencode", "session", "delete", sess->session_id, NULL},
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, NULL, NULL);
  }

  if (sess->has_session && sess->tmpdir_path != NULL) {
    /* Remove tmpdir recursively */
    g_autofree char *cmd = g_strdup_printf("rm -rf %s", sess->tmpdir_path);

    g_spawn_command_line_async(cmd, NULL);
  }

  g_hash_table_remove(state->sessions, tab_id);
}

static TabSession *get_or_create_session(ProviderState *state,
                                         const char *tab_id) {
  TabSession *sess = g_hash_table_lookup(state->sessions, tab_id);

  if (sess != NULL)
    return sess;

  sess = g_new0(TabSession, 1);
  g_hash_table_insert(state->sessions, g_strdup(tab_id), sess);
  return sess;
}

/* ── subprocess management ─────────────────────────────────────── */

static gboolean on_child_stdout(GIOChannel *source, GIOCondition cond,
                                gpointer user_data) {
  ProviderState *state = user_data;
  char buf[4096];
  gsize bytes_read;
  g_autoptr(GError) error = NULL;

  if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
    g_warning("opencode-provider: child stdout closed");
    return G_SOURCE_REMOVE;
  }

  if (!g_io_channel_read_chars(source, buf, sizeof(buf) - 1, &bytes_read,
                               &error)) {
    if (state->active_tab_id != NULL)
      write_error_event(state->active_tab_id, error->message);
    return G_SOURCE_REMOVE;
  }

  if (bytes_read > 0) {
    buf[bytes_read] = '\0';
    g_string_append_len(state->child_output_buf, buf, bytes_read);
  }

  return G_SOURCE_CONTINUE;
}

static void on_child_exit(GPid pid, gint status, gpointer user_data) {
  ProviderState *state = user_data;
  g_autofree char *text_output = NULL;
  g_autofree char *session_id = NULL;
  const char *tab_id = state->active_tab_id;

  (void)status;

  g_spawn_close_pid(pid);

  if (state->child_stdout_watch > 0) {
    g_source_remove(state->child_stdout_watch);
    state->child_stdout_watch = 0;
  }

  if (state->child_stdout_fd >= 0) {
    close(state->child_stdout_fd);
    state->child_stdout_fd = -1;
  }

  if (tab_id == NULL)
    return;

  /* Process output */
  if (state->child_output_buf != NULL && state->child_output_buf->len > 0) {
    const char *output = state->child_output_buf->str;

    /* Check for non-JSON output (error paths) */
    if (output[0] != '{') {
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        write_error_event(tab_id, output);
        goto done;
      }
      /* Raw text fallback */
      write_event(tab_id, "chunk", output);
      write_event(tab_id, "done", output);
      goto done;
    }

    text_output = parse_ndjson(output, &session_id);

    /* Store session ID */
    if (session_id != NULL) {
      TabSession *sess = g_hash_table_lookup(state->sessions, tab_id);

      if (sess != NULL) {
        g_free(sess->session_id);
        sess->session_id = g_strdup(session_id);
      }
    }

    if (text_output != NULL && text_output[0] != '\0')
      write_event(tab_id, "chunk", text_output);

    write_event(tab_id, "done", text_output != NULL ? text_output : "");
  } else {
    write_event(tab_id, "done", "");
  }

done:
  state->active_tab_id = NULL;
  state->child_pid = 0;
}

static gboolean spawn_opencode(ProviderState *state, const char *tab_id,
                               char **argv, GIOFunc stdout_cb,
                               GChildWatchFunc exit_cb) {
  (void)tab_id;
  GPid pid;
  int stdout_fd, stdin_fd, stderr_fd;
  g_autoptr(GError) error = NULL;
  GIOChannel *channel;

  if (!g_spawn_async_with_pipes(
          NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
          NULL, NULL, &pid, &stdin_fd, &stdout_fd, &stderr_fd, &error)) {
    g_warning("opencode-provider: spawn failed: %s", error->message);
    write_error_event(tab_id, error->message);
    return FALSE;
  }

  state->child_pid = pid;
  state->child_stdout_fd = stdout_fd;
  state->child_stdin_fd = stdin_fd;
  state->child_stderr_fd = stderr_fd;
  state->child_output_buf = g_string_new(NULL);
  state->submit_start = g_get_monotonic_time();

  /* Close stdin immediately (opencode doesn't need it) */
  close(stdin_fd);
  state->child_stdin_fd = -1;

  /* Set up stdout watch */
  channel = g_io_channel_unix_new(stdout_fd);
  g_io_channel_set_encoding(channel, NULL, NULL);
  state->child_stdout_watch =
      g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, stdout_cb, state);
  g_io_channel_unref(channel);

  /* Watch for child exit */
  g_child_watch_add(pid, exit_cb, state);

  /* Close stderr (we don't read it) */
  close(stderr_fd);
  state->child_stderr_fd = -1;

  return TRUE;
}

/* ── request handlers ──────────────────────────────────────────── */

static gboolean handle_initialize(ProviderState *state, JsonNode *params) {
  JsonObject *obj;
  JsonNode *config_node;
  const char *bin_path = NULL;

  if (params != NULL && !JSON_NODE_HOLDS_VALUE(params)) {
    obj = json_node_get_object(params);

    if (json_object_has_member(obj, "config")) {
      config_node = json_object_get_member(obj, "config");
      if (config_node != NULL && !JSON_NODE_HOLDS_VALUE(config_node)) {
        JsonObject *cfg = json_node_get_object(config_node);

        if (json_object_has_member(cfg, "opencode_path"))
          bin_path = json_object_get_string_member(cfg, "opencode_path");
      }
    }
  }

  if (bin_path == NULL || bin_path[0] == '\0')
    bin_path = DEFAULT_OPENCODE_PATH;

  if (g_str_has_prefix(bin_path, "~/"))
    state->opencode_bin =
        g_build_filename(g_get_home_dir(), bin_path + 2, NULL);
  else
    state->opencode_bin = g_strdup(bin_path);

  fprintf(stderr, "opencode-provider: using %s\n", state->opencode_bin);

  return TRUE;
}

static gboolean handle_submit(ProviderState *state, JsonNode *params) {
  JsonObject *obj;
  const char *tab_id, *query, *model, *agent;
  gboolean is_follow_up;
  GPtrArray *args;
  char **argv;
  g_autofree char *tmpdir_path = NULL;
  TabSession *sess;

  if (params == NULL || JSON_NODE_HOLDS_VALUE(params))
    return FALSE;

  obj = json_node_get_object(params);

  if (!json_object_has_member(obj, "tab_id") ||
      !json_object_has_member(obj, "query"))
    return FALSE;

  tab_id = json_object_get_string_member(obj, "tab_id");
  query = json_object_get_string_member(obj, "query");
  model = json_object_has_member(obj, "model")
              ? json_object_get_string_member(obj, "model")
              : NULL;
  agent = json_object_has_member(obj, "agent")
              ? json_object_get_string_member(
                    json_node_get_object(json_object_get_member(obj, "agent")),
                    "name")
              : NULL;
  is_follow_up = json_object_has_member(obj, "is_follow_up") &&
                 json_object_get_boolean_member(obj, "is_follow_up");

  if (state->active_tab_id != NULL)
    cancel_current(state);

  state->active_tab_id = g_strdup(tab_id);

  /* Session management */
  sess = get_or_create_session(state, tab_id);

  /* Build tmpdir path */
  tmpdir_path = g_strdup_printf("%s/%s", TMPDIR_PREFIX, tab_id);

  if (is_follow_up && sess->has_session) {
    fprintf(stderr, "opencode-provider: follow-up session=%s dir=%s\n",
            sess->session_id, tmpdir_path);
  } else {
    if (sess->has_session && sess->tmpdir_path != NULL) {
      g_autofree char *rm_cmd = g_strdup_printf("rm -rf %s", sess->tmpdir_path);
      g_spawn_command_line_async(rm_cmd, NULL);
    }
    g_clear_pointer(&sess->session_id, g_free);
    g_free(sess->tmpdir_path);
    sess->tmpdir_path = g_strdup(tmpdir_path);
    if (g_mkdir_with_parents(tmpdir_path, 0700) == 0)
      sess->has_session = TRUE;
  }

  /* Build effective query */
  g_autofree char *effective_query = NULL;

  if (sess->session_id == NULL && is_follow_up &&
      json_object_has_member(obj, "messages")) {
    JsonArray *messages = json_object_get_array_member(obj, "messages");
    GString *ctx = g_string_new("[Previous conversation:\n");

    if (messages != NULL) {
      for (guint i = 0; i < json_array_get_length(messages); i++) {
        JsonNode *msg_node = json_array_get_element(messages, i);
        JsonObject *msg_obj;
        const char *role, *content;

        if (msg_node == NULL || JSON_NODE_HOLDS_VALUE(msg_node))
          continue;
        msg_obj = json_node_get_object(msg_node);
        role = json_object_has_member(msg_obj, "role")
                   ? json_object_get_string_member(msg_obj, "role")
                   : "user";
        content = json_object_has_member(msg_obj, "content")
                      ? json_object_get_string_member(msg_obj, "content")
                      : "";
        g_string_append_printf(ctx, "%s: %s\n", role, content);
      }
    }
    g_string_append(ctx, "]\n\n");
    g_string_append(ctx, query);
    effective_query = g_string_free(ctx, FALSE);
  }

  /* Build CLI arguments */
  args = g_ptr_array_new();
  g_ptr_array_add(args, g_strdup(state->opencode_bin));
  g_ptr_array_add(args, g_strdup("run"));
  g_ptr_array_add(args, g_strdup("--format"));
  g_ptr_array_add(args, g_strdup("json"));

  if (is_follow_up && sess->session_id != NULL) {
    g_ptr_array_add(args, g_strdup("--session"));
    g_ptr_array_add(args, g_strdup(sess->session_id));
  }

  if (sess->has_session && sess->tmpdir_path != NULL) {
    g_ptr_array_add(args, g_strdup("--dir"));
    g_ptr_array_add(args, g_strdup(sess->tmpdir_path));
  }

  if (model != NULL && model[0] != '\0' && g_strcmp0(model, "None") != 0) {
    g_ptr_array_add(args, g_strdup("--model"));
    g_ptr_array_add(args, g_strdup(model));
  }

  if (agent != NULL && agent[0] != '\0' && g_strcmp0(agent, "None") != 0) {
    g_ptr_array_add(args, g_strdup("--agent"));
    g_ptr_array_add(args, g_strdup(agent));
  }

  g_ptr_array_add(args, g_strdup("--"));
  g_ptr_array_add(args,
                  g_strdup(effective_query != NULL ? effective_query : query));

  g_ptr_array_add(args, NULL);
  argv = (char **)g_ptr_array_free(args, FALSE);

  /* Spawn opencode CLI */
  spawn_opencode(state, tab_id, argv, on_child_stdout, on_child_exit);
  g_strfreev(argv);

  return TRUE;
}

/* ── cancel ────────────────────────────────────────────────────── */

static void cancel_current(ProviderState *state) {
  if (state->child_pid <= 0)
    return;

  kill(state->child_pid, SIGTERM);

  /* Don't clean up the watch — on_child_exit handles that */
  state->child_pid = 0;
}

static gboolean handle_cancel(ProviderState *state) {
  if (state->active_tab_id != NULL) {
    const char *tab_id = state->active_tab_id;

    cancel_current(state);

    /* Also clean up stdout watch */
    if (state->child_stdout_watch > 0) {
      g_source_remove(state->child_stdout_watch);
      state->child_stdout_watch = 0;
    }
    if (state->child_stdout_fd >= 0) {
      close(state->child_stdout_fd);
      state->child_stdout_fd = -1;
    }

    write_error_event(tab_id, "");
    state->active_tab_id = NULL;
  }

  return TRUE;
}

static gboolean handle_cleanup(ProviderState *state, const char *tab_id) {
  if (tab_id == NULL)
    return FALSE;

  if (state->active_tab_id != NULL &&
      g_strcmp0(state->active_tab_id, tab_id) == 0)
    cancel_current(state);

  cleanup_tab_session(state, tab_id);
  return TRUE;
}

/* ── main ──────────────────────────────────────────────────────── */

static gboolean on_stdin_ready(GIOChannel *source, GIOCondition cond,
                               gpointer user_data) {
  ProviderState *state = user_data;
  g_autofree char *line = NULL;
  gsize len;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *msg;
  JsonNode *root;
  const char *method;
  JsonNode *params;
  int id;

  if (cond & (G_IO_HUP | G_IO_ERR)) {
    g_main_loop_quit(g_main_loop_new(NULL, FALSE));
    return G_SOURCE_REMOVE;
  }

  /* Read one line */
  {
    GIOStatus status;

    status = g_io_channel_read_line(source, &line, &len, NULL, &error);
    if (status != G_IO_STATUS_NORMAL) {
      if (status == G_IO_STATUS_EOF) {
        g_main_loop_quit(g_main_loop_new(NULL, FALSE));
      }
      return G_SOURCE_REMOVE;
    }
  }

  /* Strip trailing newline */
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    line[--len] = '\0';

  if (line[0] == '\0')
    return G_SOURCE_CONTINUE;

  /* Parse JSON */
  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, line, -1, &error)) {
    g_warning("opencode-provider: parse error: %s", error->message);
    return G_SOURCE_CONTINUE;
  }

  root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_HOLDS_VALUE(root))
    return G_SOURCE_CONTINUE;

  msg = root; /* borrowed from parser */
  method = json_rpc_get_method(msg);
  params = json_rpc_get_params(msg);
  id = json_rpc_get_id(msg);

  /* Handle shutdown (no response expected) */
  if (method != NULL && g_strcmp0(method, "provider/shutdown") == 0) {
    g_main_loop_quit(g_main_loop_new(NULL, FALSE));
    return G_SOURCE_REMOVE;
  }

  /* Handle request methods */
  if (method != NULL && id >= 0) {
    gboolean handled = TRUE;

    if (g_strcmp0(method, "provider/initialize") == 0) {
      handled = handle_initialize(state, params);
    } else if (g_strcmp0(method, "provider/submit") == 0) {
      handled = handle_submit(state, params);
    } else if (g_strcmp0(method, "provider/cancel") == 0) {
      handled = handle_cancel(state);
    } else if (g_strcmp0(method, "provider/cleanup_session") == 0) {
      const char *tab_id = NULL;

      if (params != NULL && !JSON_NODE_HOLDS_VALUE(params)) {
        JsonObject *o = json_node_get_object(params);

        if (json_object_has_member(o, "tab_id"))
          tab_id = json_object_get_string_member(o, "tab_id");
      }
      handled = handle_cleanup(state, tab_id);
    } else {
      /* Unknown method */
      g_autoptr(JsonNode) err =
          json_rpc_make_error(-32601, "Method not found", id);

      json_rpc_write(stdout, err, NULL);
      fflush(stdout);
      return G_SOURCE_CONTINUE;
    }

    /* Send response */
    if (handled) {
      g_autoptr(JsonNode) resp = json_rpc_make_ok(id);

      json_rpc_write(stdout, resp, NULL);
      fflush(stdout);
    } else {
      g_autoptr(JsonNode) err =
          json_rpc_make_error(-32000, "Handler failed", id);

      json_rpc_write(stdout, err, NULL);
      fflush(stdout);
    }
  }

  return G_SOURCE_CONTINUE;
}

int main(void) {
  ProviderState state;
  GIOChannel *stdin_channel;
  GMainLoop *loop;

  memset(&state, 0, sizeof(state));
  state.child_pid = 0;
  state.child_stdout_fd = -1;
  state.child_stdin_fd = -1;
  state.child_stderr_fd = -1;
  state.sessions =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, session_free);

  /* Set binary stdout for JSON-RPC */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* Read requests from stdin */
  stdin_channel = g_io_channel_unix_new(STDIN_FILENO);
  g_io_channel_set_encoding(stdin_channel, NULL, NULL);
  g_io_add_watch(stdin_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin_ready,
                 &state);
  g_io_channel_unref(stdin_channel);

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);

  /* Cleanup */
  if (state.child_pid > 0)
    cancel_current(&state);
  g_hash_table_destroy(state.sessions);
  g_free(state.opencode_bin);
  g_free(state.active_tab_id);
  if (state.child_output_buf != NULL)
    g_string_free(state.child_output_buf, TRUE);

  return 0;
}
