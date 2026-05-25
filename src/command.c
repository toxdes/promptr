#include "command.h"
#include "window.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>

struct CallbackData {
  AppWindow *win;
  gint64 start_time;
  GSubprocess *subprocess;
  GCancellable *cancellable;
  CommandCallback callback;
  Tab *tab;
};

static void child_setup(gpointer user_data) {
  (void)user_data;
  setsid();
}

static void communicate_cb(GObject *source, GAsyncResult *result,
                           gpointer user_data);

static char **build_argv(const char *opencode_bin, const char *model,
                         const char *agent, const char *query,
                         const char *workdir, gboolean follow_up);

void command_execute(Tab *tab, const char *model, const char *agent,
                     const char *query, const char *workdir,
                     const char *opencode_bin, gboolean follow_up,
                     CommandCallback callback) {
  GSubprocessLauncher *launcher;
  GSubprocess *proc;
  GError *error = NULL;
  char **argv;
  struct CallbackData *cbdata;

  argv = build_argv(opencode_bin, model, agent, query, workdir, follow_up);

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);

  proc = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv,
                                      &error);

  g_object_unref(launcher);
  g_strfreev(argv);

  if (proc == NULL) {
    char *errmsg;

    errmsg = g_strdup_printf("Failed to spawn: %s", error->message);
    if (callback != NULL)
      callback(tab, NULL, errmsg, 0, -1, TRUE);
    g_free(errmsg);
    g_error_free(error);
    return;
  }

  tab->subprocess = proc;
  tab->cancellable = g_cancellable_new();
  tab->start_time = g_get_monotonic_time();

  cbdata = g_new(struct CallbackData, 1);
  cbdata->win = tab->win;
  cbdata->start_time = tab->start_time;
  cbdata->subprocess = tab->subprocess;
  cbdata->cancellable = tab->cancellable;
  cbdata->callback = callback;
  cbdata->tab = tab_ref(tab);

  g_subprocess_communicate_utf8_async(proc, NULL, tab->cancellable,
                                      communicate_cb, cbdata);
}

void command_cancel(Tab *tab) {
  const char *ident;
  pid_t pid;

  if (tab->subprocess == NULL)
    return;

  ident = g_subprocess_get_identifier(tab->subprocess);
  pid = (pid_t)strtoll(ident, NULL, 10);

  if (tab->cancellable != NULL)
    g_cancellable_cancel(tab->cancellable);

  kill(-pid, SIGKILL);
}

/* ── internal ──────────────────────────────────────────────────── */

static void communicate_cb(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  GSubprocess *proc = G_SUBPROCESS(source);
  struct CallbackData *cbdata = user_data;
  AppWindow *win = cbdata->win;
  gint64 start_time = cbdata->start_time;
  GSubprocess *sub = cbdata->subprocess;
  GCancellable *canc = cbdata->cancellable;
  CommandCallback callback = cbdata->callback;
  Tab *tab = cbdata->tab;
  g_autoptr(GError) error = NULL;
  g_autofree char *stdout_str = NULL;
  g_autofree char *stderr_str = NULL;
  gint64 elapsed;
  gboolean success;
  gboolean destroyed;
  int exit_code;

  elapsed = g_get_monotonic_time() - start_time;

  success = g_subprocess_communicate_utf8_finish(proc, result, &stdout_str,
                                                 &stderr_str, &error);

  if (success) {
    g_subprocess_wait(proc, NULL, NULL);
    exit_code = g_subprocess_get_exit_status(proc);
  } else {
    exit_code = -1;
  }

  destroyed = win->destroyed;

  g_free(cbdata);

  if (destroyed) {
    if (callback != NULL)
      callback(tab, NULL, NULL, elapsed, exit_code, FALSE);
    tab_unref(tab);
    return;
  }

  g_clear_object(&sub);
  g_clear_object(&canc);
  tab->subprocess = NULL;
  tab->cancellable = NULL;

  if (!success) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      if (callback != NULL)
        callback(tab, stdout_str, stderr_str, elapsed, exit_code, FALSE);
    } else if (g_subprocess_get_if_signaled(proc)) {
      if (callback != NULL)
        callback(tab, stdout_str, stderr_str, elapsed, exit_code, FALSE);
    } else {
      const char *msg;
      char *full;

      msg = error != NULL ? error->message : "Unknown error";
      full = g_strdup_printf("Process error: %s", msg);
      if (callback != NULL)
        callback(tab, full, stderr_str, elapsed, exit_code, TRUE);
      g_free(full);
    }
    return;
  }

  if (callback != NULL)
    callback(tab, stdout_str != NULL ? stdout_str : "",
             stderr_str != NULL ? stderr_str : "", elapsed, exit_code, TRUE);
  tab_unref(tab);
}

static char **build_argv(const char *opencode_bin, const char *model,
                         const char *agent, const char *query,
                         const char *workdir, gboolean follow_up) {
  GPtrArray *args;

  args = g_ptr_array_new();
  g_ptr_array_add(args, g_strdup(opencode_bin));
  g_ptr_array_add(args, g_strdup("run"));

  if (follow_up)
    g_ptr_array_add(args, g_strdup("--continue"));

  if (workdir != NULL && workdir[0] != '\0') {
    g_ptr_array_add(args, g_strdup("--dir"));
    g_ptr_array_add(args, g_strdup(workdir));
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
  if (query != NULL)
    g_ptr_array_add(args, g_strdup(query));
  else
    g_ptr_array_add(args, g_strdup(""));

  g_ptr_array_add(args, NULL);

  return (char **)g_ptr_array_free(args, FALSE);
}
