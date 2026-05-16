#include "command.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>

struct CallbackData {
  AppWindow *win;
  CommandCallback callback;
};

static void child_setup(gpointer user_data) {
  (void)user_data;
  setsid();
}

static void communicate_cb(GObject *source, GAsyncResult *result,
                           gpointer user_data);

static char **build_argv(const char *opencode_bin, const char *model,
                         const char *agent, const char *query,
                         const char *workdir);

void command_execute(AppWindow *win, const char *model, const char *agent,
                     const char *query, const char *workdir,
                     const char *opencode_bin, CommandCallback callback) {
  GSubprocessLauncher *launcher;
  GSubprocess *proc;
  GError *error = NULL;
  char **argv;
  struct CallbackData *cbdata;

  argv = build_argv(opencode_bin, model, agent, query, workdir);

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
      callback(win, NULL, errmsg, 0, -1, TRUE);
    g_free(errmsg);
    g_error_free(error);
    return;
  }

  win->subprocess = proc;
  win->start_time = g_get_monotonic_time();
  win->cancellable = g_cancellable_new();

  cbdata = g_new(struct CallbackData, 1);
  cbdata->win = win;
  cbdata->callback = callback;

  g_subprocess_communicate_utf8_async(proc, NULL, win->cancellable,
                                      communicate_cb, cbdata);
}

void command_cancel(AppWindow *win) {
  const char *ident;
  pid_t pid;

  if (win->subprocess == NULL)
    return;

  if (win->cancellable != NULL)
    g_cancellable_cancel(win->cancellable);

  ident = g_subprocess_get_identifier(win->subprocess);
  pid = (pid_t)strtoll(ident, NULL, 10);
  kill(-pid, SIGKILL);
}

/* ── internal ──────────────────────────────────────────────────── */

static void communicate_cb(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  GSubprocess *proc = G_SUBPROCESS(source);
  struct CallbackData *cbdata = user_data;
  AppWindow *win = cbdata->win;
  CommandCallback callback = cbdata->callback;
  g_autoptr(GError) error = NULL;
  g_autofree char *stdout_str = NULL;
  g_autofree char *stderr_str = NULL;
  gint64 elapsed;
  gboolean success;
  gboolean destroyed;
  int exit_code;

  g_free(cbdata);

  elapsed = g_get_monotonic_time() - win->start_time;

  success = g_subprocess_communicate_utf8_finish(proc, result, &stdout_str,
                                                 &stderr_str, &error);

  exit_code = g_subprocess_get_exit_status(proc);

  destroyed = win->destroyed;

  g_clear_object(&win->subprocess);
  g_clear_object(&win->cancellable);

  if (destroyed) {
    if (callback != NULL)
      callback(win, NULL, NULL, elapsed, exit_code, FALSE);
    return;
  }

  if (!success) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      if (callback != NULL)
        callback(win, stdout_str, stderr_str, elapsed, exit_code, FALSE);
    } else if (g_subprocess_get_if_signaled(proc)) {
      if (callback != NULL)
        callback(win, stdout_str, stderr_str, elapsed, exit_code, FALSE);
    } else {
      const char *msg;
      char *full;

      msg = error != NULL ? error->message : "Unknown error";
      full = g_strdup_printf("Process error: %s", msg);
      if (callback != NULL)
        callback(win, full, stderr_str, elapsed, exit_code, TRUE);
      g_free(full);
    }
    return;
  }

  if (callback != NULL)
    callback(win, stdout_str != NULL ? stdout_str : "",
             stderr_str != NULL ? stderr_str : "", elapsed, exit_code, TRUE);
}

static char **build_argv(const char *opencode_bin, const char *model,
                         const char *agent, const char *query,
                         const char *workdir) {
  GPtrArray *args;

  args = g_ptr_array_new();
  g_ptr_array_add(args, g_strdup(opencode_bin));
  g_ptr_array_add(args, g_strdup("run"));

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

  if (query != NULL)
    g_ptr_array_add(args, g_strdup(query));
  else
    g_ptr_array_add(args, g_strdup(""));

  g_ptr_array_add(args, NULL);

  return (char **)g_ptr_array_free(args, FALSE);
}
