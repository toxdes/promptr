#include "../src/plugin-manager.h"
#include <glib.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  plugin_manager_scan();
  g_assert_null(plugin_manager_get_manifest("bad-secret-schema"));
  const char *names[] = {"opencode", "openrouter"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    PluginManifest *manifest = plugin_manager_get_manifest(names[i]);
    g_assert_nonnull(manifest);
    g_assert_nonnull(manifest->command_path);
    g_assert_true(g_str_has_prefix(manifest->command_path, argv[1]));
    g_assert_true(
        g_file_test(manifest->command_path, G_FILE_TEST_IS_EXECUTABLE));
  }
  plugin_manager_shutdown();
  return 0;
}
