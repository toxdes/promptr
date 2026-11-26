#ifndef TAB_H
#define TAB_H

#include "configfile.h"
#include "provider.h"
#include <gtk/gtk.h>

typedef struct _AppWindow AppWindow;

typedef enum {
  STATE_IDLE = 0,
  STATE_LOADING,
  STATE_FINISHED,
  STATE_CANCELED,
  STATE_FAILED,
} AppState;

typedef struct {
  char *id;
  char *name;
  gboolean has_activity;
  gboolean is_open;
  AppWindow *win;
  GtkWidget *page;
  ThreadStore *store;
  gboolean hydrated;
  gboolean loading_history;
  gboolean restoring_draft;
  gboolean refresh_again;
  gboolean close_requested;
  gboolean storage_failed;
  guint view_generation;
  char *selected_agent;
  char *selected_model;
  char *project_dir;
  char *draft;
  char *request_id;
  char *request_profile;
  gboolean history_more;
  gint64 history_cursor;
  guint64 history_before;
  guint64 rendered_bytes;

  /* Present only while a request is pending. */
  Provider *provider;
  char *provider_name; /* Stable provider profile ID, or unresolved legacy name.
                        */

  GtkWidget *prompt_view;
  GtkWidget *placeholder_label;
  GtkWidget *agent_dropdown;
  GtkWidget *model_dropdown;
  GtkWidget *agent_text_label;
  GtkWidget *model_text_label;
  gboolean paned_positioned;
  GtkWidget *submit_btn;
  GtkWidget *cancel_btn;
  GtkWidget *spinner;
  GtkWidget *output_scroll;
  GtkWidget *output_view;
  GtkWidget *copy_btn;
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

  gboolean follow_up;
  gboolean follow_up_active;
  gboolean defaults_applied;

  GCancellable *cancellable;
  char *cmd_string;
  AppState state;

  char *marked_lines_str;

  GtkWidget *tab_btn;
  GtkWidget *tab_label;
  GtkWidget *close_btn_tab;
  GtkWidget *status_dot;
  GtkWidget *tab_name_label;
  GtkWidget *rename_entry;
  gboolean has_unseen_output;
  int ref_count;
} Tab;

Tab *tab_new(AppWindow *win, const char *name);
Tab *tab_ref(Tab *tab);
void tab_unref(Tab *tab);
Tab *tab_load(AppWindow *win, const char *uuid);
gboolean tab_save(Tab *tab);
GKeyFile *tab_read_metadata(const char *uuid);
void tab_capture_input(Tab *tab);
void tab_release_view(Tab *tab);
void tab_delete_saved(const char *uuid);

void tab_auto_rename(Tab *tab);

GtkWidget *tab_create_widgets(Tab *tab, AppWindow *win);
GtkWidget *tab_create_label(Tab *tab, int idx, AppWindow *win);
char *get_selected_text(GtkWidget *dropdown);
void set_selected_text(GtkWidget *dropdown, const char *text);

#endif
