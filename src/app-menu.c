#include "app-menu.h"
#include "shortcuts.h"

static void item(GMenu *menu, RuntimeConfig *config, const char *label,
                 const char *action, const char *key) {
  g_autoptr(GMenuItem) entry = g_menu_item_new(label, action);
  g_autofree char *accel = key != NULL ? shortcut_value(config, key) : NULL;
  if (accel != NULL && *accel)
    g_menu_item_set_attribute(entry, "accel", "s", accel);
  g_menu_append_item(menu, entry);
}

static GMenu *section(GMenu *parent) {
  GMenu *menu = g_menu_new();
  g_menu_append_section(parent, NULL, G_MENU_MODEL(menu));
  return menu;
}

GMenuModel *app_menu_create(RuntimeConfig *config, GMenu **providers,
                            GMenu **agents, GMenu **models) {
  GMenu *root = g_menu_new();
  g_autoptr(GMenu) file = g_menu_new();
  g_autoptr(GMenu) edit = g_menu_new();
  g_autoptr(GMenu) view = g_menu_new();
  g_autoptr(GMenu) conversation = g_menu_new();
  g_autoptr(GMenu) help = g_menu_new();
  g_autoptr(GMenu) tabs = section(file);
  item(tabs, config, "New tab", "win.new_tab", "kb_new_tab");
  item(tabs, config, "Close tab", "win.close_tab", "kb_close_tab");
  item(tabs, config, "Reopen closed tab", "win.restore_tab", "kb_restore_tab");
  item(tabs, config, "Saved conversations…", "win.history_list", NULL);
  g_autoptr(GMenu) exit = section(file);
  item(exit, config, "Close window", "win.close", "kb_close");
  item(exit, config, "Quit", "win.quit", "kb_quit");

  g_autoptr(GMenu) clipboard = section(edit);
  item(clipboard, config, "Copy marked lines", "win.copy_marked",
       "kb_copy_marked");
  item(clipboard, config, "Focus prompt", "win.focus_prompt",
       "kb_focus_prompt");
  g_autoptr(GMenu) settings = section(edit);
  item(settings, config, "Preferences…", "win.manage_providers", NULL);

  g_autoptr(GMenu) panels = section(view);
  item(panels, config, "Change panel layout", "win.toggle_layout", "kb_layout");
  item(panels, config, "Undock output", "win.popout", "kb_popout");
  g_autoptr(GMenu) bars = section(view);
  item(bars, config, "Menu bar", "win.menu_bar_visible", "kb_menu_bar");
  item(bars, config, "Status bar", "win.status_bar_visible", "kb_status_bar");
  g_autoptr(GMenu) logs = section(view);
  item(logs, config, "Session log", "win.session_log", "kb_log");

  g_autoptr(GMenu) request = section(conversation);
  item(request, config, "Send", "win.submit", "kb_submit");
  item(request, config, "Stop response", "win.cancel", "kb_cancel");
  item(request, config, "Follow up", "win.toggle_follow_up",
       "kb_follow_up_toggle");
  g_autoptr(GMenu) selections = section(conversation);
  g_autoptr(GMenu) provider = g_menu_new();
  g_menu_append_submenu(selections, "Provider", G_MENU_MODEL(provider));
  *providers = provider;
  g_autoptr(GMenu) agent = g_menu_new();
  g_autoptr(GMenu) agent_cycle = section(agent);
  item(agent_cycle, config, "Next agent", "win.cycle_agent_next",
       "kb_cycle_agent_next");
  item(agent_cycle, config, "Previous agent", "win.cycle_agent_prev",
       "kb_cycle_agent_prev");
  item(agent_cycle, config, "Choose Markdown file…", "win.choose_agent", NULL);
  g_autoptr(GMenu) agent_items = section(agent);
  *agents = agent_items;
  g_menu_append_submenu(selections, "Agent", G_MENU_MODEL(agent));
  g_autoptr(GMenu) model = g_menu_new();
  g_autoptr(GMenu) model_cycle = section(model);
  item(model_cycle, config, "Next model", "win.cycle_model_next",
       "kb_cycle_model_next");
  item(model_cycle, config, "Previous model", "win.cycle_model_prev",
       "kb_cycle_model_prev");
  item(model_cycle, config, "Enter model ID…", "win.manual_model", NULL);
  g_autoptr(GMenu) model_items = section(model);
  *models = model_items;
  g_menu_append_submenu(selections, "Model", G_MENU_MODEL(model));
  g_autoptr(GMenu) project = g_menu_new();
  item(project, config, "Choose directory…", "win.choose_project", NULL);
  item(project, config, "Clear directory", "win.clear_project", NULL);
  g_menu_append_submenu(selections, "Project", G_MENU_MODEL(project));
  g_autoptr(GMenu) history = section(conversation);
  item(history, config, "Load older messages", "win.older_history", NULL);
  item(history, config, "Latest messages", "win.latest_history", NULL);

  item(help, config, "Keyboard shortcuts", "win.shortcuts", "kb_shortcuts");
  item(help, config, "About Promptr", "win.about", NULL);
  g_menu_append_submenu(root, "_File", G_MENU_MODEL(file));
  g_menu_append_submenu(root, "_Edit", G_MENU_MODEL(edit));
  g_menu_append_submenu(root, "_View", G_MENU_MODEL(view));
  g_menu_append_submenu(root, "_Conversation", G_MENU_MODEL(conversation));
  g_menu_append_submenu(root, "_Help", G_MENU_MODEL(help));
  return G_MENU_MODEL(root);
}
