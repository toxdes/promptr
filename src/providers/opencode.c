#include "command.h"
#include "config.h"
#include "configfile.h"
#include "provider.h"
#include "tab.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

/* ── per-tab session state ────────────────────────────────────── */

typedef struct {
  gboolean has_session;
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

/* ── vtable ───────────────────────────────────────────────────── */

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
  oci->sessions =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
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

  (void)history;
  (void)n_history;
  (void)cancellable;
  (void)error;

  sess = g_hash_table_lookup(oci->sessions, tab);
  if (sess == NULL) {
    sess = g_new0(OpenCodeSession, 1);
    g_hash_table_insert(oci->sessions, tab, sess);
  }

  if (is_follow_up && sess->has_session) {
    /* Reuse existing tmpdir — opencode picks up history from --dir */
  } else {
    if (sess->has_session && tab->tmpdir_path != NULL)
      remove_dir(tab->tmpdir_path);
    if (g_mkdir_with_parents(tab->tmpdir_path, 0700) == 0)
      sess->has_session = TRUE;
  }

  args = g_ptr_array_new();
  g_ptr_array_add(args, g_strdup(oci->opencode_bin));
  g_ptr_array_add(args, g_strdup("run"));

  if (is_follow_up)
    g_ptr_array_add(args, g_strdup("--continue"));

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
  g_ptr_array_add(args, g_strdup(query != NULL ? query : ""));

  g_ptr_array_add(args, NULL);
  argv = (char **)g_ptr_array_free(args, FALSE);

  req = g_new0(OpenCodeRequest, 1);
  req->callback = callback;
  req->user_data = callback_data;

  /* Remove old active request if any */
  if (g_hash_table_contains(oci->active, tab))
    g_hash_table_remove(oci->active, tab);
  g_hash_table_insert(oci->active, tab, req);

  command_execute_argv(tab, argv, opencode_internal_cb);
  g_strfreev(argv);

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

  if (tab == NULL || tab->win == NULL)
    return;

  oci = tab->provider != NULL ? tab->provider->impl : NULL;
  if (oci == NULL)
    return;

  req = g_hash_table_lookup(oci->active, tab);
  if (req == NULL)
    return;

  g_hash_table_steal(oci->active, tab);

  if (!exited_cleanly) {
    /* Empty error_msg signals cancel (process was killed or interrupted) */
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
}

/* ── display string ──────────────────────────────────────────── */

static char *opencode_get_display_string(void *impl, const char *query,
                                         const char *model, const char *agent,
                                         gboolean is_follow_up) {
  OpenCodeImpl *oci = impl;
  GString *s;

  s = g_string_new(oci->opencode_bin);
  g_string_append(s, " run");
  if (is_follow_up)
    g_string_append(s, " --continue");
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
