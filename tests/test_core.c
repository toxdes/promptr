#include "../lib/promptr-protocol/jsonrpc.h"
#include "../src/agent-context.h"
#include "../src/app-diagnostics.h"
#include "../src/app-menu.h"
#include "../src/plugin-manager.h"
#include "../src/plugin-runner.h"
#include "../src/provider-profile.h"
#include "../src/shortcuts.h"
#include "fake_keyring.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

static const char *fixture;

static void shortcut_catalog(void) {
  gsize count;
  const ShortcutDefinition *definitions = shortcut_definitions(&count);
  g_autofree char *defaults = runtime_config_defaults();
  g_autoptr(GKeyFile) config = g_key_file_new();
  g_assert_true(
      g_key_file_load_from_data(config, defaults, -1, G_KEY_FILE_NONE, NULL));
  g_autoptr(GHashTable) keys = g_hash_table_new(g_str_hash, g_str_equal);
  for (gsize i = 0; i < count; i++) {
    g_assert_true(g_hash_table_add(keys, (gpointer)definitions[i].key));
    g_autofree char *value =
        g_key_file_get_string(config, "preferences", definitions[i].key, NULL);
    g_assert_cmpstr(value, ==, definitions[i].fallback);
    g_assert_nonnull(definitions[i].label);
    g_assert_nonnull(definitions[i].group);
  }
  g_auto(GStrv) default_keys =
      g_key_file_get_keys(config, "preferences", NULL, NULL);
  gsize configured_count = 0;
  for (guint i = 0; default_keys[i] != NULL; i++)
    if (g_str_has_prefix(default_keys[i], "kb_")) {
      g_assert_true(g_hash_table_contains(keys, default_keys[i]));
      configured_count++;
    }
  g_assert_cmpuint(configured_count, ==, count);
  RuntimeConfig *runtime = runtime_config_load();
  const char *custom_keys[] = {"kb_focus_prompt"};
  g_autofree char *original = shortcut_value(runtime, custom_keys[0]);
  const char *custom_values[] = {"<Alt>k"};
  g_assert_true(runtime_config_save_values(runtime, custom_keys, custom_values,
                                           G_N_ELEMENTS(custom_keys), NULL));
  g_autofree char *custom = shortcut_human(runtime, custom_keys[0]);
  g_assert_cmpstr(custom, ==, "alt+k");
  const char *empty_values[] = {""};
  g_assert_true(runtime_config_save_values(runtime, custom_keys, empty_values,
                                           G_N_ELEMENTS(custom_keys), NULL));
  g_autofree char *empty = shortcut_human(runtime, custom_keys[0]);
  g_assert_cmpstr(empty, ==, "");
  const char *original_values[] = {original};
  g_assert_true(runtime_config_save_values(
      runtime, custom_keys, original_values, G_N_ELEMENTS(custom_keys), NULL));
  runtime_config_free(runtime);
  const char *input[] = {"<Control><Shift>c", "<Alt>Return", "", NULL};
  const char *expected[] = {"ctrl+shift+c", "alt+return", "", ""};
  for (guint i = 0; i < G_N_ELEMENTS(input); i++) {
    g_autofree char *human = accel_to_human(input[i]);
    g_assert_cmpstr(human, ==, expected[i]);
  }
}

static void toolkit_diagnostics(void) {
  const char *known[] = {
      "Cannot get portal org.freedesktop.portal.Inhibit version: "
      "GDBus.Error:org.freedesktop.DBus.Error.InvalidArgs: "
      "No such interface “org.freedesktop.portal.Inhibit”",
      "gdk_frame_timings_presented() called on discarded frame.",
      "gdk_frame_timings_discarded() called on already presented frame.",
      "Duplicate call to gdk_frame_timings_presented() with different values: "
      "presentation time is 116.669ms off from expected",
      "Duplicate call to gdk_frame_timings_presented() with different values: "
      "presentation time is -50.-125ms off from expected",
      "Duplicate call to gdk_frame_timings_presented() with different values: "
      "presentation time is 0.-999ms off from expected",
  };
  for (guint i = 0; i < G_N_ELEMENTS(known); i++) {
    GLogField fields[] = {{"GLIB_DOMAIN", "Gdk", -1},
                          {"MESSAGE", known[i], -1}};
    g_assert_true(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                               G_N_ELEMENTS(fields), 4, 24, 1));
    g_assert_true(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                               G_N_ELEMENTS(fields), 4, 24, 0));
    g_assert_true(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                               G_N_ELEMENTS(fields), 4, 24, 2));
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_WARNING, fields, G_N_ELEMENTS(fields), 4, 24, 3));
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_WARNING, fields, G_N_ELEMENTS(fields), 4, 26, 0));
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_CRITICAL, fields, G_N_ELEMENTS(fields), 4, 24, 1));
    g_assert_false(
        app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                                     fields, G_N_ELEMENTS(fields), 4, 24, 1));
    g_assert_false(
        app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION,
                                     fields, G_N_ELEMENTS(fields), 4, 24, 1));
    fields[0].value = "Promptr";
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_WARNING, fields, G_N_ELEMENTS(fields), 4, 24, 1));
    fields[0].value = "Gdk";
    fields[1].length = strlen(known[i]) + 1;
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_WARNING, fields, G_N_ELEMENTS(fields), 4, 24, 1));
    g_autofree char *bounded = g_memdup2(known[i], strlen(known[i]));
    fields[1].value = bounded;
    fields[1].length = strlen(known[i]);
    g_assert_true(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                               G_N_ELEMENTS(fields), 4, 24, 1));
    fields[1].length--;
    g_assert_false(app_diagnostics_is_gtk_noise(
        G_LOG_LEVEL_WARNING, fields, G_N_ELEMENTS(fields), 4, 24, 1));
  }
  const char *invalid_offsets[] = {
      "",
      "50",
      ".0",
      "50.",
      "-.-",
      "+50.0",
      "50.0e2",
      "NaN",
      "inf",
      "50.0000",
      "10000000000000.0",
      "50.0ms off from expected extra",
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid_offsets); i++) {
    g_autofree char *message =
        g_strconcat("Duplicate call to gdk_frame_timings_presented() with "
                    "different values: "
                    "presentation time is ",
                    invalid_offsets[i], "ms off from expected", NULL);
    GLogField invalid_fields[] = {{"GLIB_DOMAIN", "Gdk", -1},
                                  {"MESSAGE", message, -1}};
    g_assert_false(
        app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, invalid_fields,
                                     G_N_ELEMENTS(invalid_fields), 4, 24, 1));
    invalid_fields[1].length = strlen(message);
    g_assert_false(
        app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, invalid_fields,
                                     G_N_ELEMENTS(invalid_fields), 4, 24, 1));
  }
  GLogField fields[] = {{"GLIB_DOMAIN", "Gdk", -1},
                        {"MESSAGE", "Unexpected rendering failure", -1}};
  g_assert_false(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                              G_N_ELEMENTS(fields), 4, 24, 1));
  fields[1].value = "Cannot get portal org.freedesktop.portal.Inhibit version: "
                    "Permission denied";
  g_assert_false(app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, fields,
                                              G_N_ELEMENTS(fields), 4, 24, 1));
  g_assert_false(
      app_diagnostics_is_gtk_noise(G_LOG_LEVEL_WARNING, NULL, 0, 4, 24, 1));
}

static void protocol_validation(void) {
  const char *invalid[] = {
      "[]",
      "null",
      "{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{}}",
      "{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"result\":{}}",
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},\"error\":{}}",
      "{\"jsonrpc\":\"2.0\",\"method\":1}"};
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    g_assert_true(json_parser_load_from_data(parser, invalid[i], -1, NULL));
    g_assert_false(json_rpc_validate(json_parser_get_root(parser), &error));
    g_assert_nonnull(error);
  }
  FILE *file = tmpfile();
  g_assert_nonnull(file);
  g_autofree char *large = g_strnfill(100000, 'x');
  g_autoptr(JsonObject) object = json_object_new();
  json_object_set_string_member(object, "text", large);
  g_autoptr(JsonNode) p = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(p, object);
  g_autoptr(JsonNode) request =
      json_rpc_make_request("large", g_steal_pointer(&p), 1);
  g_assert_true(json_rpc_write(file, request, NULL));
  rewind(file);
  g_autoptr(JsonNode) read = json_rpc_read(file, NULL);
  g_assert_nonnull(read);
  fclose(file);
}

static gboolean menu_has_action(GMenuModel *menu, const char *action) {
  for (int i = 0; i < g_menu_model_get_n_items(menu); i++) {
    g_autofree char *value = NULL;
    if (g_menu_model_get_item_attribute(menu, i, G_MENU_ATTRIBUTE_ACTION, "s",
                                        &value) &&
        g_strcmp0(value, action) == 0)
      return TRUE;
    const char *links[] = {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU};
    for (guint j = 0; j < G_N_ELEMENTS(links); j++) {
      g_autoptr(GMenuModel) child =
          g_menu_model_get_item_link(menu, i, links[j]);
      if (child != NULL && menu_has_action(child, action))
        return TRUE;
    }
  }
  return FALSE;
}

static void preferences_save(void) {
  RuntimeConfig *config = runtime_config_load();
  g_autofree char *path = g_build_filename(g_get_user_config_dir(),
                                           "promptr-debug", "config", NULL);
  g_autoptr(GKeyFile) disk = g_key_file_new();
  g_assert_true(
      g_key_file_load_from_file(disk, path, G_KEY_FILE_KEEP_COMMENTS, NULL));
  /* Simulate an external edit after this configuration snapshot was loaded. */
  g_key_file_set_string(disk, "preferences", "layout", "vertical");
  g_key_file_set_comment(disk, "preferences", "layout", "external comment",
                         NULL);
  g_autofree char *external = g_key_file_to_data(disk, NULL, NULL);
  g_assert_true(g_file_set_contents(path, external, -1, NULL));
  const char *keys[] = {"query_font", "answer_font", "editor_line_numbers"};
  const char *values[] = {"Serif Bold 14", "Monospace 12", "0"};
  g_assert_true(runtime_config_save_values(config, keys, values,
                                           G_N_ELEMENTS(keys), NULL));
  RuntimeConfig *reloaded = runtime_config_load();
  g_autofree char *unrelated =
      runtime_config_get_string(reloaded, "layout", "");
  g_assert_cmpstr(unrelated, ==, "vertical");
  g_autofree char *query =
      runtime_config_get_string(reloaded, "query_font", "");
  g_assert_cmpstr(query, ==, "Serif Bold 14");
  g_assert_false(
      runtime_config_get_bool(reloaded, "editor_line_numbers", TRUE));
  runtime_config_free(reloaded);
  g_autofree char *saved = NULL;
  g_assert_true(g_file_get_contents(path, &saved, NULL, NULL));
  g_assert_nonnull(strstr(saved, "external comment"));
  /* A malformed on-disk file must not be overwritten or alter live values. */
  g_assert_true(g_file_set_contents(path, "[broken", -1, NULL));
  const char *changed[] = {"Serif 20", "Monospace 20", "1"};
  g_autoptr(GError) error = NULL;
  g_assert_false(runtime_config_save_values(config, keys, changed,
                                            G_N_ELEMENTS(keys), &error));
  g_assert_nonnull(error);
  g_autofree char *unchanged =
      runtime_config_get_string(config, "query_font", "");
  g_assert_cmpstr(unchanged, ==, "Serif Bold 14");
  g_autofree char *malformed = NULL;
  g_assert_true(g_file_get_contents(path, &malformed, NULL, NULL));
  g_assert_cmpstr(malformed, ==, "[broken");
  g_assert_true(g_file_set_contents(path, saved, -1, NULL));
  runtime_config_free(config);
}

static void preferences_font_migration(void) {
  g_autofree char *path = g_build_filename(g_get_user_config_dir(),
                                           "promptr-debug", "config", NULL);
  const char *legacy = "[preferences]\neditor_font_family=Noto Sans "
                       "Mono\nprompt_font_size=15\noutput_font_size=12\n";
  g_assert_true(g_file_set_contents(path, legacy, -1, NULL));
  RuntimeConfig *config = runtime_config_load();
  g_autofree char *query = runtime_config_get_string(config, "query_font", "");
  g_autofree char *answer =
      runtime_config_get_string(config, "answer_font", "");
  g_assert_cmpstr(query, ==, "Noto Sans Mono 15");
  g_assert_cmpstr(answer, ==, "Noto Sans Mono 12");
  runtime_config_free(config);
  config = runtime_config_load();
  g_autofree char *persisted =
      runtime_config_get_string(config, "query_font", "");
  g_assert_cmpstr(persisted, ==, query);
  runtime_config_free(config);
}

static void native_menus(void) {
  RuntimeConfig *config = runtime_config_load();
  GMenu *providers, *agents, *models;
  g_autoptr(GMenuModel) menu =
      app_menu_create(config, &providers, &agents, &models);
  const char *labels[] = {"_File", "_Edit", "_View", "_Conversation", "_Help"};
  g_assert_cmpint(g_menu_model_get_n_items(menu), ==, G_N_ELEMENTS(labels));
  for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
    g_autofree char *label = NULL;
    g_assert_true(g_menu_model_get_item_attribute(
        menu, i, G_MENU_ATTRIBUTE_LABEL, "s", &label));
    g_assert_cmpstr(label, ==, labels[i]);
    g_autoptr(GMenuModel) submenu =
        g_menu_model_get_item_link(menu, i, G_MENU_LINK_SUBMENU);
    g_assert_nonnull(submenu);
    g_assert_null(g_menu_model_get_item_attribute_value(
        menu, i, G_MENU_ATTRIBUTE_ACTION, NULL));
    if (i == 1)
      g_assert_true(menu_has_action(submenu, "win.manage_providers"));
  }
  g_assert_true(G_IS_MENU(providers));
  g_assert_true(G_IS_MENU(agents));
  g_assert_true(G_IS_MENU(models));
  g_assert_true(menu_has_action(menu, "win.cancel"));
  runtime_config_free(config);
}

static ThreadStore *new_store(char **id) {
  *id = g_uuid_string_random();
  return thread_store_new(*id);
}

static void storage_roundtrip(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "request", "question", TRUE, NULL));
  g_assert_true(thread_store_chunk(store, "hello ", NULL));
  g_assert_true(thread_store_chunk(store, "world", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_assert_true(thread_store_set_resume(store, "profile-a", "opaque", NULL));
  g_autofree char *state = thread_store_get_resume(store, "profile-a");
  g_assert_cmpstr(state, ==, "opaque");
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 2);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 1), "content"),
      ==, "hello world");
  g_clear_pointer(&store, thread_store_unref);
  store = thread_store_new(id);
  g_autoptr(ThreadPage) page = thread_store_page(store, -1, 0, NULL);
  g_assert_nonnull(strstr(page->text, "hello world"));
  g_assert_cmpstr(page->text, ==, "hello world");
  g_assert_true(thread_store_begin(store, "replacement", "fresh", TRUE, NULL));
  g_autofree char *cleared = thread_store_get_resume(store, "profile-a");
  g_assert_null(cleared);
  g_autoptr(JsonArray) fresh = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(fresh), ==, 1);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(fresh, 0), "content"), ==,
      "fresh");
}

static void storage_output_presentation(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "one", "first question", TRUE, NULL));
  g_assert_true(thread_store_chunk(store, "first answer", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_assert_true(
      thread_store_begin(store, "two", "second question", FALSE, NULL));
  g_autoptr(ThreadPage) pending = thread_store_page(store, -1, 0, NULL);
  g_assert_cmpstr(pending->text, ==,
                  "\n---\nQ.1: first question\nA: first answer");
  g_assert_cmpint(pending->stream_chars, ==, 0);
  g_assert_true(thread_store_chunk(store, "second answer", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_assert_true(
      thread_store_begin(store, "three", "current question", FALSE, NULL));
  g_assert_true(thread_store_chunk(store, "current answer", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_autoptr(ThreadPage) page = thread_store_page(store, -1, 0, NULL);
  g_assert_cmpstr(page->text, ==,
                  "current answer\n---\nQ.2: second question\nA: second "
                  "answer\n---\nQ.1: first question\nA: first answer");
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 6);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 4), "content"),
      ==, "current question");
}

static void storage_paged_turns(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  guint questions[61] = {0}, answers[G_N_ELEMENTS(questions)] = {0};
  for (guint i = 0; i < G_N_ELEMENTS(questions); i++) {
    g_autofree char *query = g_strdup_printf("<question:%03u>", i);
    g_autofree char *answer = g_strdup_printf("<answer:%03u>", i);
    g_assert_true(thread_store_begin(store, query, query, i == 0, NULL));
    g_assert_true(thread_store_chunk(store, answer, NULL));
    g_assert_true(thread_store_finish(store, "completed", NULL));
  }
  gint64 cursor = -1;
  guint64 before = 0;
  guint pages = 0;
  for (;;) {
    g_autoptr(ThreadPage) page = thread_store_page(store, cursor, before, NULL);
    g_assert_nonnull(page);
    for (guint i = 0; i < G_N_ELEMENTS(questions); i++) {
      g_autofree char *query = g_strdup_printf("<question:%03u>", i);
      g_autofree char *answer = g_strdup_printf("<answer:%03u>", i);
      questions[i] += strstr(page->text, query) != NULL;
      answers[i] += strstr(page->text, answer) != NULL;
    }
    pages++;
    if (!page->more)
      break;
    g_assert_cmpuint(pages, <, 5);
    cursor = page->next_message;
    before = page->before;
  }
  g_assert_cmpuint(pages, >=, 2);
  for (guint i = 0; i < G_N_ELEMENTS(questions); i++) {
    g_assert_cmpuint(answers[i], ==, 1);
    g_assert_cmpuint(questions[i], ==,
                     i == G_N_ELEMENTS(questions) - 1 ? 0 : 1);
  }
}

static void storage_large_page(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(
      thread_store_begin(store, "large", "large question", TRUE, NULL));
  g_autofree char *chunk = g_strnfill(60000, 'a');
  for (int i = 0; i < 12; i++)
    g_assert_true(thread_store_chunk(store, chunk, NULL));
  g_assert_true(thread_store_finish(store, "cancelled", NULL));
  guint pages = 0;
  guint64 seen = 0, before = 0;
  gint64 cursor = -1;
  do {
    g_autoptr(ThreadPage) page = thread_store_page(store, cursor, before, NULL);
    g_assert_nonnull(page);
    g_assert_cmpuint(strlen(page->text), <=, 256 * 1024);
    for (const char *p = page->text; *p; p++)
      if (*p == 'a')
        seen++;
    cursor = page->next_message;
    before = page->before;
    pages++;
    if (!page->more)
      break;
    g_assert_cmpuint(pages, <, 10);
  } while (TRUE);
  g_assert_cmpuint(pages, >=, 3);
  g_assert_cmpuint(seen, >=, 720000);
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(strlen(protocol_string(
                       json_array_get_object_element(messages, 1), "content")),
                   ==, 720000);
}

static void storage_split_records(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_autofree char *query = g_strnfill(200000, 'q');
  g_autofree char *answer = g_strnfill(300000, 'a');
  g_assert_true(thread_store_begin(store, "split", query, TRUE, NULL));
  g_assert_true(thread_store_chunk(store, answer, NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_clear_pointer(&store, thread_store_unref);
  g_autofree char *dir = thread_store_directory();
  g_autofree char *index = g_strdup_printf("%s/%s.index", dir, id);
  g_assert_cmpint(g_unlink(index), ==, 0);
  store = thread_store_new(id);
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_nonnull(messages);
  g_assert_cmpuint(json_array_get_length(messages), ==, 2);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 0), "content"),
      ==, query);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 1), "content"),
      ==, answer);
}

static void storage_oversized_legacy(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_autofree char *dir = thread_store_directory();
  g_autofree char *legacy = g_strdup_printf("%s/%s.conf", dir, id);
  g_autofree char *text_path = g_strdup_printf("%s/%s.txt", dir, id);
  g_autofree char *journal = g_strdup_printf("%s/%s.journal", dir, id);
  g_autofree char *index = g_strdup_printf("%s/%s.index", dir, id);
  g_assert_true(g_file_set_contents(legacy, "[tab]\nversion=1\n", -1, NULL));
  FILE *file = g_fopen(text_path, "w");
  g_assert_nonnull(file);
  g_autofree char *block = g_strnfill(1024 * 1024, 'a');
  for (guint i = 0; i < 65; i++)
    g_assert_cmpuint(fwrite(block, 1, 1024 * 1024, file), ==, 1024 * 1024);
  g_assert_cmpint(fclose(file), ==, 0);
  g_assert_true(thread_store_import(store, NULL));
  GStatBuf before, after;
  g_assert_cmpint(g_stat(journal, &before), ==, 0);
  g_clear_pointer(&store, thread_store_unref);
  g_assert_cmpint(g_unlink(index), ==, 0);
  store = thread_store_new(id);
  g_autoptr(ThreadPage) page = thread_store_page(store, -1, 0, NULL);
  g_assert_nonnull(page);
  g_assert_true(page->more);
  g_assert_cmpuint(strlen(page->text), <=, 256 * 1024);
  g_assert_cmpint(g_stat(journal, &after), ==, 0);
  g_assert_cmpint(before.st_size, ==, after.st_size);
}

static void storage_recovery_retry(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "retry", "question", TRUE, NULL));
  g_assert_true(thread_store_chunk(store, "answer", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_clear_pointer(&store, thread_store_unref);
  g_autofree char *dir = thread_store_directory();
  g_autofree char *journal = g_strdup_printf("%s/%s.journal", dir, id);
  FILE *file = g_fopen(journal, "a");
  g_assert_nonnull(file);
  fputs("broken", file);
  fclose(file);
  store = thread_store_new(id);
  g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonArray) failed = thread_store_messages(store, &error);
  g_assert_cmpint(g_chmod(dir, 0700), ==, 0);
  g_assert_null(failed);
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*Recovered interrupted*");
  g_autoptr(JsonArray) messages = thread_store_messages(store, &error);
  g_test_assert_expected_messages();
  g_assert_no_error(error);
  g_assert_nonnull(messages);
  g_assert_cmpuint(json_array_get_length(messages), ==, 2);
}

static void storage_corrupt_index(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "request", "question", TRUE, NULL));
  g_assert_true(thread_store_chunk(store, "answer", NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  g_autofree char *dir = thread_store_directory(),
                  *name = g_strconcat(id, ".index", NULL);
  g_autofree char *path = g_build_filename(dir, name, NULL), *original = NULL;
  g_assert_true(g_file_get_contents(path, &original, NULL, NULL));
  g_autofree char *damaged = g_strdup(original);
  damaged[strlen(damaged) - 2] = '0';
  g_assert_true(g_file_set_contents(path, damaged, -1, NULL));
  g_clear_pointer(&store, thread_store_unref);
  store = thread_store_new(id);
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_nonnull(messages);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 1), "content"),
      ==, "answer");
  g_autofree char *rebuilt = NULL;
  g_assert_true(g_file_get_contents(path, &rebuilt, NULL, NULL));
  g_assert_cmpstr(rebuilt, ==, original);
}

static void storage_recovery(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(
      thread_store_begin(store, "interrupted", "question", TRUE, NULL));
  g_assert_true(thread_store_chunk(store, "partial", NULL));
  g_clear_pointer(&store, thread_store_unref);
  g_autofree char *dir = thread_store_directory(),
                  *name = g_strconcat(id, ".journal", NULL);
  g_autofree char *path = g_build_filename(dir, name, NULL);
  FILE *file = g_fopen(path, "a");
  g_assert_nonnull(file);
  fputs("{broken", file);
  fclose(file);
  store = thread_store_new(id);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*Recovered interrupted conversation*");
  g_autoptr(ThreadPage) page = thread_store_page(store, -1, 0, NULL);
  g_test_assert_expected_messages();
  g_assert_nonnull(page);
  g_assert_nonnull(strstr(page->text, "partial"));
  g_assert_true(thread_store_begin(store, "next", "continue", FALSE, NULL));
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 3);
  g_assert_true(thread_store_delete(id, NULL));
  g_autoptr(GDir) remaining = g_dir_open(dir, 0, NULL);
  const char *file_name;
  while ((file_name = g_dir_read_name(remaining)) != NULL)
    g_assert_false(g_str_has_prefix(file_name, id));
}

static void storage_migration(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_autofree char *dir = thread_store_directory();
  g_mkdir_with_parents(dir, 0700);
  g_autofree char *name = g_strconcat(id, ".conf", NULL),
                  *path = g_build_filename(dir, name, NULL);
  const char *legacy =
      "[tab]\nversion=2\nname=Old\n[messages]\ncount=3\n[msg_0]\nrole="
      "system\ncontent=rules\n[msg_1]\nrole=user\ncontent=old "
      "question\n[msg_2]\nrole=assistant\ncontent=old answer\n";
  g_assert_true(g_file_set_contents(path, legacy, -1, NULL));
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 3);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 0), "role"), ==,
      "system");
  g_autofree char *original = NULL;
  g_assert_true(g_file_get_contents(path, &original, NULL, NULL));
  g_assert_cmpstr(original, ==, legacy);
  g_clear_pointer(&store, thread_store_unref);
  store = thread_store_new(id);
  g_autoptr(JsonArray) again = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(again), ==, 3);
}

static void storage_v1_and_partial_migration(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_autofree char *dir = thread_store_directory();
  g_mkdir_with_parents(dir, 0700);
  g_autofree char *name = g_strconcat(id, ".conf", NULL),
                  *path = g_build_filename(dir, name, NULL);
  const char *legacy =
      "[tab]\nversion=1\n[follow_up]\nturn=2\nlast_query=last\nlast_output="
      "last answer\n[history_0]\nquery=recent\noutput=recent "
      "answer\n[history_1]\nquery=oldest\noutput=oldest answer\n";
  g_assert_true(g_file_set_contents(path, legacy, -1, NULL));
  g_autoptr(JsonArray) messages = thread_store_messages(store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 6);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 0), "content"),
      ==, "oldest");
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 4), "content"),
      ==, "last");
  g_autofree char *partial_id = NULL;
  g_autoptr(ThreadStore) partial = new_store(&partial_id);
  g_autofree char *partial_name = g_strconcat(partial_id, ".conf", NULL),
                  *partial_path = g_build_filename(dir, partial_name, NULL);
  g_autofree char *txt_name = g_strconcat(partial_id, ".txt", NULL),
                  *txt_path = g_build_filename(dir, txt_name, NULL);
  g_file_set_contents(partial_path,
                      "[tab]\nversion=2\n[messages]\ncount=1\n[msg_0]\nrole="
                      "user\ncontent=question\n",
                      -1, NULL);
  g_file_set_contents(
      txt_path, "partial answer\n---\nQ.1: older question\nA: older answer", -1,
      NULL);
  g_autoptr(JsonArray) recovered = thread_store_messages(partial, NULL);
  g_assert_cmpuint(json_array_get_length(recovered), ==, 2);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(recovered, 1), "content"),
      ==, "partial answer");
  g_autoptr(ThreadPage) page = thread_store_page(partial, -1, 0, NULL);
  g_assert_nonnull(strstr(page->text, "interrupted"));
}

static void storage_unsaved_output(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "request", "question", TRUE, NULL));
  g_autofree char *dir = thread_store_directory(),
                  *name = g_strconcat(id, ".journal", NULL);
  g_autofree char *path = g_build_filename(dir, name, NULL),
                  *backup = g_strconcat(path, ".saved", NULL);
  g_assert_cmpint(g_rename(path, backup), ==, 0);
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_false(thread_store_chunk(store, "unsaved output", &error));
  g_clear_error(&error);
  g_assert_true(thread_store_has_unsaved(store));
  g_assert_false(thread_store_finish(store, "cancelled", &error));
  g_clear_error(&error);
  g_assert_false(thread_store_begin(store, "another", "fresh", TRUE, &error));
  g_clear_error(&error);
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_cmpint(g_rename(backup, path), ==, 0);
  g_autoptr(ThreadPage) page = thread_store_page(store, -1, 0, &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(page->text, "unsaved output"));
  g_assert_false(thread_store_has_unsaved(store));
}

static void storage_utf8_pages(void) {
  g_autofree char *id = NULL;
  g_autoptr(ThreadStore) store = new_store(&id);
  g_assert_true(thread_store_begin(store, "utf8", "q", TRUE, NULL));
  g_autoptr(GString) chunk = g_string_new(NULL);
  for (guint i = 0; i < 2000; i++)
    g_string_append(chunk, "\xce\xbb\xe6\xbc\xa2x");
  for (guint i = 0; i < 60; i++)
    g_assert_true(thread_store_chunk(store, chunk->str, NULL));
  g_assert_true(thread_store_finish(store, "completed", NULL));
  guint lambda = 0, han = 0, x = 0;
  gint64 cursor = -1;
  guint64 before = 0;
  for (guint n = 0; n < 10; n++) {
    g_autoptr(ThreadPage) page = thread_store_page(store, cursor, before, NULL);
    g_assert_nonnull(page);
    g_assert_true(g_utf8_validate(page->text, -1, NULL));
    for (const char *p = page->text; *p; p = g_utf8_next_char(p)) {
      gunichar c = g_utf8_get_char(p);
      lambda += c == 0x3bb;
      han += c == 0x6f22;
      x += c == 'x';
    }
    cursor = page->next_message;
    before = page->before;
    if (!page->more)
      break;
  }
  g_assert_cmpuint(lambda, ==, 120000);
  g_assert_cmpuint(han, ==, 120000);
  g_assert_cmpuint(x, ==, 120000);
}

typedef struct {
  Provider *provider;
  GCancellable *cancel;
  ThreadStore *store;
  GString *output;
  char *error;
  guint terminal;
  gboolean succeeded;
  char **models;
} Request;

static void request_event(gpointer source, const ProviderEvent *event,
                          gpointer data) {
  (void)data;
  Request *r = source;
  if (event->type == PROVIDER_EVENT_CHUNK)
    g_string_append(r->output, event->output);
  else {
    r->terminal++;
    r->succeeded = event->type == PROVIDER_EVENT_DONE;
    r->error = g_strdup(event->error_msg);
    r->models = g_strdupv(event->models);
  }
}

static void start_request(Request *r, const char *mode, const char *id,
                          gboolean follow_up, gboolean test, gboolean models) {
  g_autoptr(ProviderProfile) p = provider_profile_new("fixture");
  g_free(p->command);
  p->command = g_find_program_in_path("python3");
  p->arguments = g_new0(char *, 3);
  p->arguments[0] = g_strdup(fixture);
  p->arguments[1] = g_strdup(mode);
  json_object_set_string_member(p->settings, "api_key", "fake-private-value");
  r->provider = plugin_runner_create_profile(p);
  r->cancel = g_cancellable_new();
  r->output = g_string_new(NULL);
  r->store = test || models ? NULL : thread_store_new(id);
  g_autofree char *request_id = g_uuid_string_random();
  ProviderRequest q = {.thread_id = id,
                       .request_id = request_id,
                       .query = "question",
                       .model = "fixture/model",
                       .agent_name = "fixture-agent",
                       .agent_content =
                           "Full Markdown context\nWith more instructions.",
                       .project_dir = "/tmp/a project",
                       .store = r->store,
                       .follow_up = follow_up,
                       .connection_test = test,
                       .discover_models = models};
  g_assert_true(r->provider->vtable->submit(r->provider->impl, r, &q, r->cancel,
                                            request_event, NULL, NULL));
}

static void wait_requests(Request *a, Request *b) {
  gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  while (a->terminal == 0 || (b != NULL && b->terminal == 0)) {
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_usleep(1000);
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
  }
  g_assert_cmpuint(a->terminal, ==, 1);
  if (b != NULL)
    g_assert_cmpuint(b->terminal, ==, 1);
}

static void clear_request(Request *r) {
  r->provider->vtable->destroy(r->provider->impl);
  g_free(r->provider);
  g_object_unref(r->cancel);
  thread_store_unref(r->store);
  g_string_free(r->output, TRUE);
  g_free(r->error);
  g_strfreev(r->models);
}

static void runner_isolation(void) {
  g_autofree char *one = g_uuid_string_random(), *two = g_uuid_string_random();
  Request a = {0}, b = {0};
  start_request(&a, "stream", one, FALSE, FALSE, FALSE);
  start_request(&b, "late", two, FALSE, FALSE, FALSE);
  while (a.output->len == 0) {
    g_main_context_iteration(NULL, TRUE);
  }
  g_cancellable_cancel(a.cancel);
  wait_requests(&a, &b);
  g_assert_false(a.succeeded);
  g_assert_true(b.succeeded);
  g_assert_cmpstr(b.output->str, ==, "pong");
  g_autoptr(JsonArray) messages = thread_store_messages(a.store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 2);
  g_assert_nonnull(strstr(
      protocol_string(json_array_get_object_element(messages, 1), "content"),
      "partial"));
  clear_request(&a);
  clear_request(&b);
}

static void runner_history(void) {
  g_autofree char *id = g_uuid_string_random();
  Request first = {0};
  start_request(&first, "normal", id, FALSE, FALSE, FALSE);
  wait_requests(&first, NULL);
  clear_request(&first);
  Request second = {0};
  start_request(&second, "echo", id, TRUE, FALSE, FALSE);
  wait_requests(&second, NULL);
  g_assert_true(second.succeeded);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(
      json_parser_load_from_data(parser, second.output->str, -1, NULL));
  JsonObject *p = json_node_get_object(json_parser_get_root(parser));
  JsonArray *history = json_object_get_array_member(p, "messages");
  g_assert_cmpuint(json_array_get_length(history), ==, 3);
  g_assert_cmpstr(protocol_string(p, "project_dir"), ==, "/tmp/a project");
  g_assert_cmpstr(protocol_string(protocol_object(p, "agent"), "content"), ==,
                  "Full Markdown context\nWith more instructions.");
  g_assert_false(json_object_has_member(p, "query"));
  clear_request(&second);
}

static void runner_failures(void) {
  const char *modes[] = {"incompatible", "malformed",    "exit", "reject",
                         "oversize",     "secret_error", "empty"};
  for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
    g_autofree char *id = g_uuid_string_random();
    Request r = {0};
    start_request(&r, modes[i], id, FALSE, TRUE, FALSE);
    wait_requests(&r, NULL);
    g_assert_false(r.succeeded);
    g_assert_nonnull(r.error);
    g_assert_null(strstr(r.error, "fake-private-value"));
    clear_request(&r);
  }
}

static void runner_slow_start(void) {
  g_autofree char *id = g_uuid_string_random();
  Request r = {0};
  gint64 start = g_get_monotonic_time();
  start_request(&r, "stall", id, FALSE, TRUE, FALSE);
  g_assert_cmpint(g_get_monotonic_time() - start, <, G_TIME_SPAN_SECOND);
  g_cancellable_cancel(r.cancel);
  wait_requests(&r, NULL);
  g_assert_false(r.succeeded);
  clear_request(&r);
}

static void runner_deadlines(void) {
  g_autofree char *id = g_uuid_string_random();
  Request silent = {0};
  gint64 start = g_get_monotonic_time();
  start_request(&silent, "stall", id, FALSE, TRUE, FALSE);
  wait_requests(&silent, NULL);
  g_assert_false(silent.succeeded);
  g_assert_nonnull(strstr(silent.error, "timed out"));
  g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_TIME_SPAN_SECOND);
  clear_request(&silent);
  Request waiting = {0};
  start = g_get_monotonic_time();
  start_request(&waiting, "stall_reply", id, FALSE, TRUE, FALSE);
  wait_requests(&waiting, NULL);
  g_assert_false(waiting.succeeded);
  g_assert_nonnull(strstr(waiting.error, "timed out"));
  g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_TIME_SPAN_SECOND);
  clear_request(&waiting);
  Request ignores = {0};
  start_request(&ignores, "ignore_cancel", id, FALSE, TRUE, FALSE);
  /* Test mode suppresses deltas; allow the child to spawn before cancellation.
   */
  gint64 cancel_at = g_get_monotonic_time() + 150 * 1000;
  while (g_get_monotonic_time() < cancel_at) {
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_usleep(1000);
  }
  start = g_get_monotonic_time();
  g_cancellable_cancel(ignores.cancel);
  wait_requests(&ignores, NULL);
  g_assert_false(ignores.succeeded);
  g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_TIME_SPAN_SECOND);
  clear_request(&ignores);
}

static void runner_many_requests(void) {
  Request requests[12] = {0};
  for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
    g_autofree char *id = g_uuid_string_random();
    start_request(&requests[i], "stream", id, FALSE, FALSE, FALSE);
  }
  gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
  for (;;) {
    gboolean complete = TRUE;
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    for (guint i = 0; i < G_N_ELEMENTS(requests); i++)
      complete = complete && requests[i].terminal == 1;
    if (complete)
      break;
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(1000);
  }
  for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
    g_assert_true(requests[i].succeeded);
    clear_request(&requests[i]);
  }
}

static void agents(void) {
  g_autofree char *dir = g_build_filename(g_get_user_config_dir(),
                                          "promptr-debug", "agents", NULL);
  g_mkdir_with_parents(dir, 0700);
  g_autofree char *path = g_build_filename(dir, "example.md", NULL);
  const char *markdown = "# Context\n\nAll of this is delivered.\n";
  g_assert_true(g_file_set_contents(path, markdown, -1, NULL));
  g_auto(GStrv) list = agent_context_list();
  g_assert_true(g_strv_contains((const char *const *)list, "example"));
  g_autoptr(GError) error = NULL;
  g_autofree char *body = agent_context_resolve("example", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(body, ==, markdown);
  g_autofree char *absolute = agent_context_resolve(path, &error);
  g_assert_cmpstr(absolute, ==, markdown);
  g_autofree char *missing = agent_context_resolve("missing", &error);
  g_assert_null(missing);
  g_assert_nonnull(error);
}

static void runner_discovery(void) {
  g_autofree char *id = g_uuid_string_random();
  Request r = {0};
  start_request(&r, "normal", id, FALSE, FALSE, TRUE);
  wait_requests(&r, NULL);
  g_assert_true(r.succeeded);
  g_assert_cmpstr(r.models[1], ==, "fixture/second");
  clear_request(&r);
}

static void bundled_profiles(void) {
  const char *plugins[] = {"openrouter", "opencode"};
  for (guint i = 0; i < G_N_ELEMENTS(plugins); i++) {
    PluginManifest *manifest = plugin_manager_get_manifest(plugins[i]);
    g_assert_nonnull(manifest);
    g_assert_true(g_uuid_string_is_valid(manifest->profile_id));
    g_autoptr(ProviderProfile) p =
        provider_profile_load(manifest->profile_id, NULL);
    g_assert_nonnull(p);
    g_assert_true(p->bundled);
    g_assert_false(p->tested);
    g_assert_cmpstr(p->plugin, ==, plugins[i]);
    g_assert_nonnull(p->models);
    g_assert_nonnull(p->models[0]);
    g_assert_cmpuint(json_object_get_size(p->secret_refs), ==, 0);
    g_autoptr(GError) error = NULL;
    g_assert_false(provider_profile_save(p, NULL, &error));
    g_assert_nonnull(error);
    g_autoptr(GPtrArray) all = provider_profile_list();
    guint matches = 0;
    for (guint j = 0; j < all->len; j++)
      matches += g_strcmp0(((ProviderProfile *)g_ptr_array_index(all, j))->id,
                           p->id) == 0;
    g_assert_cmpuint(matches, ==, 1);
  }
}

static void missing_key_preserves_history(void) {
  PluginManifest *manifest = plugin_manager_get_manifest("openrouter");
  g_autoptr(ProviderProfile) profile =
      provider_profile_load(manifest->profile_id, NULL);
  if (!g_file_test(profile->command, G_FILE_TEST_IS_EXECUTABLE)) {
    g_test_skip("Build the bundled adapters with make debug first");
    return;
  }
  g_autofree char *id = g_uuid_string_random();
  Request r = {.provider = plugin_runner_create_profile(profile),
               .cancel = g_cancellable_new(),
               .output = g_string_new(NULL),
               .store = thread_store_new(id)};
  g_assert_true(thread_store_begin(r.store, "old-request", "keep this question",
                                   TRUE, NULL));
  g_assert_true(thread_store_chunk(r.store, "keep this answer", NULL));
  g_assert_true(thread_store_finish(r.store, "completed", NULL));
  ProviderRequest request = {.thread_id = id,
                             .request_id = "missing-key",
                             .query = "replacement",
                             .store = r.store,
                             .follow_up = FALSE};
  g_assert_true(r.provider->vtable->submit(
      r.provider->impl, &r, &request, r.cancel, request_event, NULL, NULL));
  wait_requests(&r, NULL);
  g_assert_false(r.succeeded);
  g_assert_nonnull(strstr(r.error, "No OpenRouter API key configured"));
  g_autoptr(JsonArray) messages = thread_store_messages(r.store, NULL);
  g_assert_cmpuint(json_array_get_length(messages), ==, 2);
  g_assert_cmpstr(
      protocol_string(json_array_get_object_element(messages, 0), "content"),
      ==, "keep this question");
  clear_request(&r);
}

static void profiles(void) {
  fake_keyring_reset(FALSE);
  g_autoptr(ProviderProfile) p = provider_profile_new("fixture");
  g_autoptr(GError) error = NULL;
  g_assert_false(provider_profile_save(p, NULL, &error));
  g_clear_error(&error);
  p->tested = TRUE;
  g_assert_true(provider_profile_save(p, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(ProviderProfile) saved = provider_profile_load(p->id, &error);
  g_assert_nonnull(saved);
  g_assert_no_error(error);
  p->secret_fields = g_new0(char *, 2);
  p->secret_fields[0] = g_strdup("api_key");
  json_object_set_string_member(p->settings, "api_key",
                                "never-save-this-plaintext");
  g_assert_false(provider_profile_save(p, NULL, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_autoptr(ProviderProfile) unchanged = provider_profile_load(p->id, NULL);
  g_assert_null(protocol_string(unchanged->settings, "api_key"));
  /* A declared secret field is not a keyring reference until a value was
   * actually stored. Environment-based profiles work without a secret service.
   */
  json_object_remove_member(p->settings, "api_key");
  g_assert_true(provider_profile_save(p, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(ProviderProfile) environment = provider_profile_load(p->id, &error);
  g_assert_no_error(error);
  g_autoptr(JsonObject) resolved =
      provider_profile_resolve(environment, NULL, &error);
  g_assert_nonnull(resolved);
  g_assert_no_error(error);
  g_assert_true(provider_profile_delete(environment, NULL, &error));
  g_assert_no_error(error);
}

static void profile_atomic_credentials(void) {
  fake_keyring_reset(TRUE);
  g_autoptr(ProviderProfile) original = provider_profile_new("fixture");
  original->tested = TRUE;
  original->secret_fields = g_new0(char *, 2);
  original->secret_fields[0] = g_strdup("api_key");
  json_object_set_string_member(original->settings, "api_key",
                                "old-private-key");
  json_object_set_string_member(original->settings, "endpoint",
                                "https://old.example");
  g_autoptr(GError) error = NULL;
  g_assert_true(provider_profile_save(original, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(ProviderProfile) old_snapshot =
      provider_profile_load(original->id, &error);
  g_assert_no_error(error);
  g_autoptr(JsonObject) old_values =
      provider_profile_resolve(old_snapshot, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(protocol_string(old_values, "api_key"), ==,
                  "old-private-key");
  g_assert_null(protocol_string(old_snapshot->settings, "api_key"));
  g_autoptr(ProviderProfile) update = provider_profile_copy(old_snapshot);
  json_object_set_string_member(update->settings, "api_key", "new-private-key");
  json_object_set_string_member(update->settings, "endpoint",
                                "https://new.example");
  g_autofree char *name = g_strconcat(original->id, ".json", NULL);
  g_autofree char *path = g_build_filename(
      g_get_user_config_dir(), "promptr-debug", "providers", name, NULL);
  g_autofree char *backup = g_strconcat(path, ".saved", NULL);
  g_assert_cmpint(g_rename(path, backup), ==, 0);
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_assert_false(provider_profile_save(update, NULL, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_cmpuint(fake_keyring_count(), ==, 1);
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_cmpint(g_rename(backup, path), ==, 0);
  g_autoptr(ProviderProfile) still_live =
      provider_profile_load(original->id, &error);
  g_assert_no_error(error);
  g_autoptr(JsonObject) still_values =
      provider_profile_resolve(still_live, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(protocol_string(still_values, "endpoint"), ==,
                  "https://old.example");
  g_assert_cmpstr(protocol_string(still_values, "api_key"), ==,
                  "old-private-key");
  g_assert_true(provider_profile_save(update, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(ProviderProfile) new_live =
      provider_profile_load(original->id, &error);
  g_assert_no_error(error);
  g_autoptr(JsonObject) new_values =
      provider_profile_resolve(new_live, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(protocol_string(new_values, "endpoint"), ==,
                  "https://new.example");
  g_assert_cmpstr(protocol_string(new_values, "api_key"), ==,
                  "new-private-key");
  g_autoptr(JsonObject) pending_values =
      provider_profile_resolve(old_snapshot, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(protocol_string(pending_values, "api_key"), ==,
                  "old-private-key");
  g_assert_cmpuint(fake_keyring_count(), ==, 2);
  g_assert_true(provider_profile_delete(new_live, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(fake_keyring_count(), ==, 0);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  fixture = g_getenv("PROMPTR_TEST_FIXTURE");
  g_assert_nonnull(fixture);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/protocol/validation", protocol_validation);
  g_test_add_func("/ui/native-menu-model", native_menus);
  g_test_add_func("/preferences/save", preferences_save);
  g_test_add_func("/diagnostics/gtk-compatibility", toolkit_diagnostics);
  g_test_add_func("/ui/shortcut-catalog", shortcut_catalog);
  g_test_add_func("/preferences/font-migration", preferences_font_migration);
  g_test_add_func("/storage/roundtrip", storage_roundtrip);
  g_test_add_func("/storage/output-presentation", storage_output_presentation);
  g_test_add_func("/storage/paged-turns", storage_paged_turns);
  g_test_add_func("/storage/paging", storage_large_page);
  g_test_add_func("/storage/corrupt-index", storage_corrupt_index);
  g_test_add_func("/storage/recovery", storage_recovery);
  g_test_add_func("/storage/recovery-retry", storage_recovery_retry);
  g_test_add_func("/storage/split-records", storage_split_records);
  g_test_add_func("/storage/oversized-legacy", storage_oversized_legacy);
  g_test_add_func("/storage/migration", storage_migration);
  g_test_add_func("/storage/v1-and-partial-migration",
                  storage_v1_and_partial_migration);
  g_test_add_func("/storage/unsaved-output", storage_unsaved_output);
  g_test_add_func("/storage/utf8-paging", storage_utf8_pages);
  g_test_add_func("/runner/isolation", runner_isolation);
  g_test_add_func("/runner/history", runner_history);
  g_test_add_func("/runner/failures", runner_failures);
  g_test_add_func("/runner/slow-start", runner_slow_start);
  g_test_add_func("/runner/deadlines", runner_deadlines);
  g_test_add_func("/runner/many-requests", runner_many_requests);
  g_test_add_func("/runner/discovery", runner_discovery);
  g_test_add_func("/profiles/bundled-defaults", bundled_profiles);
  g_test_add_func("/runner/missing-key-preserves-history",
                  missing_key_preserves_history);
  g_test_add_func("/profiles/keyring-failure", profiles);
  g_test_add_func("/profiles/atomic-credentials", profile_atomic_credentials);
  g_test_add_func("/agents/markdown", agents);
  int status = g_test_run();
  fake_keyring_reset(FALSE);
  return status;
}
