#include "tab.h"
#include "command.h"
#include "window.h"
#include <stdlib.h>

Tab *tab_new(AppWindow *win, int id, const char *name) {
  Tab *tab;

  tab = g_new0(Tab, 1);
  tab->win = win;
  tab->id = id;
  tab->name = g_strdup(name);
  tab->has_activity = FALSE;
  tab->state = 0; /* STATE_IDLE */
  return tab;
}

void tab_free(Tab *tab) {
  if (tab == NULL)
    return;

  if (tab->subprocess != NULL) {
    GSubprocess *proc;

    proc = g_object_ref(tab->subprocess);
    command_cancel(tab);
    g_subprocess_wait(proc, NULL, NULL);
    g_object_unref(proc);
  }

  g_list_free_full(tab->qa_history, g_free);
  g_free(tab->name);
  g_free(tab->page_name);
  g_free(tab->tab_file);
  g_free(tab->tmpdir_path);
  g_free(tab->last_query);
  g_free(tab->last_output);
  g_free(tab->cmd_string);
  g_free(tab->marked_lines_str);

  g_free(tab);
}

Tab *app_window_get_active_tab(AppWindow *win) {
  if (win->tabs == NULL || win->tabs->len == 0)
    return NULL;
  return g_ptr_array_index(win->tabs, win->active_tab_idx);
}
