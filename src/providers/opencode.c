#include "command.h"
#include "config.h"
#include "configfile.h"
#include "provider.h"
#include "tab.h"
#include "window.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

/* ── per-tab session state ────────────────────────────────────── */

typedef struct {
  gboolean has_session;
  char *session_id;
} OpenCodeSession;

typedef struct {
  char *opencode_bin;
  GHashTable *sessions; /* gpointer tab_key -> OpenCodeSession* */
  GHashTable *active;   /* gpointer tab_key -> OpenCodeRequest*   */
} OpenCodeImpl;

typedef struct {
  ProviderCallback callback;
  gpointer user_data;
} OpenCodeRequest;

/* ── forward declarations ────────────────────────────────────── */

static gboolean opencode_init(void **impl, RuntimeConfig *config,
                              GError **error);
static void opencode_destroy(void *impl);
static void opencode_cleanup_session(void *impl, gpointer tab_key);
static gboolean opencode_submit(void *impl, gpointer tab_key, const char *query,
                                const ProviderMessage *history, int n_history,
                                const char *model, const char *agent,
                                gboolean is_follow_up,
                                GCancellable *cancellable,
                                ProviderCallback callback,
                                gpointer callback_data, GError **error);
static void opencode_cancel(void *impl);
static void opencode_cancel_tab(void *impl, gpointer tab_key);
static char *opencode_get_display_string(void *impl, const char *query,
                                         const char *model, const char *agent,
                                         gboolean is_follow_up);
static char **opencode_get_models(void *impl);
static char **opencode_get_agents(void *impl);
static void opencode_internal_cb(Tab *tab, const char *output,
                                 const char *stderr_output, gint64 elapsed_us,
                                 int exit_code, gboolean exited_cleanly);
static void opencode_session_free(gpointer data);
static char *opencode_parse_ndjson(const char *output, char **out_session_id);

/* ── vtable ───────────────────────────────────────────────────── */

static void opencode_session_free(gpointer data) {
  OpenCodeSession *sess = data;

  if (sess == NULL)
    return;
  g_free(sess->session_id);
  g_free(sess);
}

ProviderVTable opencode_vtable = {
    .name = "opencode",
    .init = opencode_init,
    .destroy = opencode_destroy,
    .cleanup_session = opencode_cleanup_session,
    .submit = opencode_submit,
    .cancel = opencode_cancel,
    .cancel_tab = opencode_cancel_tab,
    .get_display_string = opencode_get_display_string,
    .get_models = opencode_get_models,
    .get_agents = opencode_get_agents,
    .needs_full_history = FALSE,
};

/* ── init / destroy ──────────────────────────────────────────── */

static gboolean opencode_init(void **impl, RuntimeConfig *config,
                              GError **error) {
  OpenCodeImpl *oci;
  g_autofree char *raw = NULL;

  (void)error;

  oci = g_new0(OpenCodeImpl, 1);
  oci->sessions = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                        opencode_session_free);
  oci->active =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

  raw = runtime_config_get_string(config, "opencode_path", OPENCODE_PATH);
  if (g_str_has_prefix(raw, "~/"))
    oci->opencode_bin = g_build_filename(g_get_home_dir(), raw + 2, NULL);
  else
    oci->opencode_bin = g_strdup(raw);

  *impl = oci;
  return TRUE;
}

static void opencode_destroy(void *impl) {
  OpenCodeImpl *oci = impl;

  if (oci == NULL)
    return;

  g_hash_table_destroy(oci->sessions);
  g_hash_table_destroy(oci->active);
  g_free(oci->opencode_bin);
  g_free(oci);
}

static void opencode_cleanup_session(void *impl, gpointer tab_key) {
  OpenCodeImpl *oci = impl;
  OpenCodeSession *sess;
  Tab *tab = tab_key;

  sess = g_hash_table_lookup(oci->sessions, tab);
  if (sess == NULL)
    return;

  if (sess->session_id != NULL) {
    /* Fire-and-forget: delete the opencode session to keep the
       database clean — safe even if session doesn't exist. */
    g_spawn_async(
        NULL,
        (char *[]){"opencode", "session", "delete", sess->session_id, NULL},
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, NULL, NULL);
  }

  if (sess->has_session && tab->tmpdir_path != NULL)
    remove_dir(tab->tmpdir_path);

  g_hash_table_remove(oci->sessions, tab);
}

/* ── submit ───────────────────────────────────────────────────── */

static gboolean opencode_submit(void *impl, gpointer tab_key, const char *query,
                                const ProviderMessage *history, int n_history,
                                const char *model, const char *agent,
                                gboolean is_follow_up,
                                GCancellable *cancellable,
                                ProviderCallback callback,
                                gpointer callback_data, GError **error) {
  OpenCodeImpl *oci = impl;
  Tab *tab = tab_key;
  GPtrArray *args;
  char **argv;
  OpenCodeSession *sess;
  OpenCodeRequest *req;
  const char *effective_query;
  g_autofree char *query_ptr = NULL;

  (void)cancellable;
  (void)error;
  (void)history;

  sess = g_hash_table_lookup(oci->sessions, tab);
  if (sess == NULL) {
    sess = g_new0(OpenCodeSession, 1);
    g_hash_table_insert(oci->sessions, tab, sess);
  }

  if (is_follow_up && sess->has_session) {
    /* Reuse existing tmpdir */
    log_append(tab->win, "opencode → follow-up session=%s dir=%s",
               sess->session_id, tab->tmpdir_path);
  } else {
    /* Start fresh: create tmpdir, clear old session_id. */
    if (sess->has_session && tab->tmpdir_path != NULL)
      remove_dir(tab->tmpdir_path);
    g_clear_pointer(&sess->session_id, g_free);
    if (g_mkdir_with_parents(tab->tmpdir_path, 0700) == 0)
      sess->has_session = TRUE;
  }

  /* Build the effective query — inject conversation history as context
     when we have history but no session (e.g. switching from another
     provider back to opencode). */
  if (sess->session_id == NULL && is_follow_up && n_history > 0) {
    GString *ctx = g_string_new("[Previous conversation:\n");

    for (int i = 0; i < n_history; i++) {
      const char *role_str, *content;

      role_str = history[i].role == PROVIDER_ROLE_USER ? "User" : "Assistant";
      content = history[i].content != NULL ? history[i].content : "";
      g_string_append_printf(ctx, "%s: %s\n", role_str, content);
    }
    g_string_append(ctx, "]\n\n");
    g_string_append(ctx, query);

    log_append(tab->win, "opencode → injected %d history messages as context",
               n_history);
    query_ptr = g_string_free(ctx, FALSE);
  }

  effective_query = query_ptr != NULL ? query_ptr : query;

  args = g_ptr_array_new();
  g_ptr_array_add(args, g_strdup(oci->opencode_bin));
  g_ptr_array_add(args, g_strdup("run"));
  g_ptr_array_add(args, g_strdup("--format"));
  g_ptr_array_add(args, g_strdup("json"));

  if (is_follow_up) {
    if (sess->session_id != NULL) {
      g_ptr_array_add(args, g_strdup("--session"));
      g_ptr_array_add(args, g_strdup(sess->session_id));
      log_append(tab->win, "opencode → using session=%s", sess->session_id);
    }
    /* No --continue fallback: context injection provides the
       conversation history without relying on opencode's session
       management, which can leak stale context across provider
       switches. */
  }

  if (sess->has_session && tab->tmpdir_path != NULL) {
    g_ptr_array_add(args, g_strdup("--dir"));
    g_ptr_array_add(args, g_strdup(tab->tmpdir_path));
  }

  if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0') {
    g_ptr_array_add(args, g_strdup("--model"));
    g_ptr_array_add(args, g_strdup(model));
  }

  if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0') {
    g_ptr_array_add(args, g_strdup("--agent"));
    g_ptr_array_add(args, g_strdup(agent));
  }

  g_ptr_array_add(args, g_strdup("--"));
  g_ptr_array_add(args,
                  g_strdup(effective_query != NULL ? effective_query : ""));

  g_ptr_array_add(args, NULL);
  argv = (char **)g_ptr_array_free(args, FALSE);

  req = g_new0(OpenCodeRequest, 1);
  req->callback = callback;
  req->user_data = callback_data;

  if (g_hash_table_contains(oci->active, tab))
    g_hash_table_remove(oci->active, tab);
  g_hash_table_insert(oci->active, tab, req);

  command_execute_argv(tab, argv, tab->tmpdir_path, opencode_internal_cb);
  g_strfreev(argv);

  log_append(tab->win, "opencode → submitted%s%s",
             sess->session_id ? " session=" : "",
             sess->session_id ? sess->session_id : "");

  /* Update display string with the actual command */
  {
    GString *ds = g_string_new(oci->opencode_bin);

    g_string_append(ds, " run --format json");
    if (sess->session_id != NULL)
      g_string_append_printf(ds, " --session %s", sess->session_id);
    if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
      g_string_append_printf(ds, " --model %s", model);
    if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
      g_string_append_printf(ds, " --agent %s", agent);
    g_string_append_printf(ds, " %s", query != NULL ? query : "");

    g_free(tab->cmd_string);
    tab->cmd_string = g_string_free(ds, FALSE);
  }

  return TRUE;
}

/* ── cancel ───────────────────────────────────────────────────── */

static void opencode_cancel(void *impl) {
  OpenCodeImpl *oci = impl;
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init(&iter, oci->active);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    Tab *tab = key;

    command_cancel(tab);
  }
  /* Don't clear the active table — let the async callback
     (communicate_cb → opencode_internal_cb) clean up the request
     and call on_provider_event → set_canceled_state. */
}

static void opencode_cancel_tab(void *impl, gpointer tab_key) {
  OpenCodeImpl *oci = impl;
  Tab *tab = tab_key;

  if (g_hash_table_contains(oci->active, tab))
    command_cancel(tab);
  /* Same rationale: async callback handles cleanup. */
}

/* ── internal callback (wraps CommandCallback → ProviderCallback) */

static void opencode_internal_cb(Tab *tab, const char *output,
                                 const char *stderr_output, gint64 elapsed_us,
                                 int exit_code, gboolean exited_cleanly) {
  OpenCodeImpl *oci;
  OpenCodeRequest *req;
  OpenCodeSession *sess;
  g_autofree char *session_id = NULL;
  g_autofree char *text_output = NULL;

  if (tab == NULL || tab->win == NULL)
    return;

  oci = tab->provider != NULL ? tab->provider->impl : NULL;
  if (oci == NULL)
    return;

  req = g_hash_table_lookup(oci->active, tab);
  if (req == NULL)
    return;

  g_hash_table_steal(oci->active, tab);

  /* For non-json output (error paths, old versions), pass raw text */
  if (output == NULL || output[0] == '\0' || output[0] != '{' ||
      exited_cleanly == FALSE) {
    if (!exited_cleanly) {
      ProviderEvent ev = {PROVIDER_EVENT_ERROR, NULL, (char *)"", elapsed_us};
      req->callback(tab, &ev, req->user_data);
      g_free(req);
      return;
    }
    if (exit_code != 0) {
      ProviderEvent ev = {
          PROVIDER_EVENT_ERROR, NULL,
          (char *)(stderr_output != NULL ? stderr_output : "Command failed"),
          elapsed_us};
      req->callback(tab, &ev, req->user_data);
      g_free(req);
      return;
    }
    /* Raw text fallback (--format default, not json) */
    {
      ProviderEvent chunk = {PROVIDER_EVENT_CHUNK,
                             (char *)(output != NULL ? output : ""), NULL,
                             elapsed_us};
      req->callback(tab, &chunk, req->user_data);
    }
    {
      ProviderEvent done = {PROVIDER_EVENT_DONE,
                            (char *)(output != NULL ? output : ""), NULL,
                            elapsed_us};
      req->callback(tab, &done, req->user_data);
    }
    g_free(req);
    return;
  }

  /* Parse NDJSON format */
  text_output = opencode_parse_ndjson(output, &session_id);

  /* Store session ID in the session state */
  sess = g_hash_table_lookup(oci->sessions, tab);
  if (sess != NULL && session_id != NULL) {
    g_free(sess->session_id);
    sess->session_id = g_strdup(session_id);
  }

  if (text_output != NULL && text_output[0] != '\0') {
    ProviderEvent chunk = {PROVIDER_EVENT_CHUNK, text_output, NULL, elapsed_us};
    req->callback(tab, &chunk, req->user_data);
  }

  {
    ProviderEvent done = {PROVIDER_EVENT_DONE,
                          text_output != NULL ? text_output : "", NULL,
                          elapsed_us};
    req->callback(tab, &done, req->user_data);
  }

  g_free(req);
}

/* ── NDJSON parser for opencode --format json ────────────────── */

static char *opencode_parse_ndjson(const char *output, char **out_session_id) {
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

    /* Capture sessionID from any event */
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
    /* type "error" — we can't emit from here, the caller handles it via
       exit_code check and the empty text output being shown as error. */
  }

  if (out_session_id != NULL)
    *out_session_id = g_strdup(session_id);

  return g_string_free(text, FALSE);
}

/* ── display string ──────────────────────────────────────────── */

static char *opencode_get_display_string(void *impl, const char *query,
                                         const char *model, const char *agent,
                                         gboolean is_follow_up) {
  OpenCodeImpl *oci = impl;

  (void)is_follow_up;
  GString *s;

  s = g_string_new(oci->opencode_bin);
  g_string_append(s, " run --format json");
  if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
    g_string_append_printf(s, " --model %s", model);
  if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
    g_string_append_printf(s, " --agent %s", agent);
  g_string_append_printf(s, " %s", query != NULL ? query : "");
  if (model != NULL && g_strcmp0(model, "None") != 0 && model[0] != '\0')
    g_string_append_printf(s, " --model %s", model);
  if (agent != NULL && g_strcmp0(agent, "None") != 0 && agent[0] != '\0')
    g_string_append_printf(s, " --agent %s", agent);
  g_string_append_printf(s, " %s", query != NULL ? query : "");

  return g_string_free(s, FALSE);
}

/* ── model / agent lists ─────────────────────────────────────── */

static char **opencode_get_models(void *impl) {
  (void)impl;
  return NULL; /* loaded from config by window.c via
                  runtime_config_get_string_list */
}

static char **opencode_get_agents(void *impl) {
  (void)impl;
  return NULL;
}
