#include "agent-context.h"
#include "config.h"

static char *agent_dir(void) {
  return g_build_filename(g_get_user_config_dir(), DATA_DIR_SUFFIX, "agents",
                          NULL);
}

char **agent_context_list(void) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(out, g_strdup("None"));
  g_autofree char *dir = agent_dir();
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  const char *name;
  while (d != NULL && (name = g_dir_read_name(d)) != NULL)
    if (g_str_has_suffix(name, ".md"))
      g_ptr_array_add(out, g_strndup(name, strlen(name) - 3));
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

char *agent_context_resolve(const char *agent, GError **error) {
  if (agent == NULL || !*agent || g_strcmp0(agent, "None") == 0)
    return NULL;
  g_autofree char *dir = agent_dir();
  g_autofree char *filename = g_strconcat(agent, ".md", NULL);
  g_autofree char *path = g_path_is_absolute(agent)
                              ? g_strdup(agent)
                              : g_build_filename(dir, filename, NULL);
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GFileInfo) info =
      g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                        G_FILE_QUERY_INFO_NONE, NULL, error);
  if (info == NULL)
    return NULL;
  if (g_file_info_get_size(info) > 1024 * 1024) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "Agent Markdown exceeds the 1 MiB context limit");
    return NULL;
  }
  char *content = NULL;
  gsize size;
  if (!g_file_get_contents(path, &content, &size, error))
    return NULL;
  if (!g_utf8_validate(content, size, NULL)) {
    g_free(content);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Agent Markdown must be UTF-8 text");
    return NULL;
  }
  return content;
}
