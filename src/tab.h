#ifndef TAB_H
#define TAB_H

#include <gtk/gtk.h>

typedef struct _AppWindow AppWindow;

typedef struct {
  int id;
  char *name;
  char *page_name;
  char *tab_file;
  char *tmpdir_path;
  gboolean has_activity;
  AppWindow *win;

  GtkWidget *prompt_view;
  GtkWidget *placeholder_label;
  GtkWidget *agent_dropdown;
  GtkWidget *model_dropdown;
  GtkWidget *submit_btn;
  GtkWidget *cancel_btn;
  GtkWidget *spinner;
  GtkWidget *output_scroll;
  GtkWidget *output_view;
  GtkWidget *copy_btn;
  GtkWidget *close_btn;
  GtkWidget *quit_btn;
  GtkWidget *marked_label;
  GtkWidget *output_label;
  GtkWidget *follow_up_check;

  int layout_mode;
  gboolean output_popped;
  GtkWidget *popout_window;
  GtkWidget *popout_btn;
  GtkWidget *prompt_btns;
  GtkWidget *agent_btns;
  GtkWidget *content_stack;
  GtkWidget *layout_paned;
  GtkWidget *pane_left;
  GtkWidget *pane_right;
  GtkWidget *layout_popped;
  GtkWidget *prompt_section;
  GtkWidget *fu_row;
  GtkWidget *agent_row;
  GtkWidget *output_section;
  GtkWidget *marked_row;
  GtkWidget *action_row;
  GtkWidget *log_btn;
  GtkWidget *log_btn_top;
  GtkWidget *shortcuts_btn;
  GtkWidget *shortcuts_btn_top;

  GList *qa_history;
  char *last_query;
  char *last_output;
  int follow_up_turn;
  gboolean follow_up;
  gboolean follow_up_active;
  gboolean defaults_applied;

  GSubprocess *subprocess;
  GCancellable *cancellable;
  char *cmd_string;
  gint64 start_time;
  int state;

  GSList *temp_dirs;

  char *marked_lines_str;

  GtkWidget *tab_btn;
  GtkWidget *tab_label;
  GtkWidget *close_btn_tab;
  GtkWidget *status_dot;
  GtkWidget *tab_name_label;
  GtkWidget *rename_entry;
  gboolean has_unseen_output;
} Tab;

Tab *tab_new(AppWindow *win, int id, const char *name);
Tab *tab_load(AppWindow *win, int id, const char *filepath);
void tab_free(Tab *tab);
void tab_save(Tab *tab);

void tab_apply_layout(Tab *tab);
void tab_toggle_popout(Tab *tab);
void tab_setup_tooltips(Tab *tab);

char *tab_get_trimmed_text(GtkWidget *text_view);

#endif
