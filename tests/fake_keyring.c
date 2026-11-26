/* Link-time substitutes isolate profile transaction tests from real
 * credentials. */
#include "fake_keyring.h"
#include <libsecret/secret.h>
#include <stdarg.h>

typedef struct {
  GHashTable *attributes;
  char *password;
} Item;
static GPtrArray *items;
static gboolean available;

static void item_free(gpointer data) {
  Item *item = data;
  g_hash_table_unref(item->attributes);
  g_free(item->password);
  g_free(item);
}
void fake_keyring_reset(gboolean enabled) {
  if (items != NULL)
    g_ptr_array_unref(items);
  items = enabled ? g_ptr_array_new_with_free_func(item_free) : NULL;
  available = enabled;
}
guint fake_keyring_count(void) { return items != NULL ? items->len : 0; }

static GHashTable *attributes(va_list args) {
  GHashTable *out =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  const char *name;
  while ((name = va_arg(args, const char *)) != NULL) {
    const char *value = va_arg(args, const char *);
    g_assert_nonnull(value);
    g_hash_table_insert(out, g_strdup(name), g_strdup(value));
  }
  return out;
}
static gboolean matches(Item *item, GHashTable *query) {
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, query);
  while (g_hash_table_iter_next(&iter, &key, &value))
    if (g_strcmp0(g_hash_table_lookup(item->attributes, key), value) != 0)
      return FALSE;
  return TRUE;
}
static gboolean check(GCancellable *cancel, GError **error) {
  if (cancel != NULL && g_cancellable_set_error_if_cancelled(cancel, error))
    return FALSE;
  if (!available) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Fixture secret service unavailable");
    return FALSE;
  }
  return TRUE;
}

gboolean __wrap_secret_password_store_sync(
    const SecretSchema *schema, const gchar *collection, const gchar *label,
    const gchar *password, GCancellable *cancel, GError **error, ...) {
  (void)schema;
  (void)collection;
  (void)label;
  if (!check(cancel, error))
    return FALSE;
  va_list args;
  va_start(args, error);
  GHashTable *query = attributes(args);
  va_end(args);
  for (guint i = 0; i < items->len; i++) {
    Item *item = g_ptr_array_index(items, i);
    if (matches(item, query)) {
      g_free(item->password);
      item->password = g_strdup(password);
      g_hash_table_unref(query);
      return TRUE;
    }
  }
  Item *item = g_new0(Item, 1);
  item->attributes = query;
  item->password = g_strdup(password);
  g_ptr_array_add(items, item);
  return TRUE;
}

gchar *__wrap_secret_password_lookup_sync(const SecretSchema *schema,
                                          GCancellable *cancel, GError **error,
                                          ...) {
  (void)schema;
  if (!check(cancel, error))
    return NULL;
  va_list args;
  va_start(args, error);
  g_autoptr(GHashTable) query = attributes(args);
  va_end(args);
  for (guint i = 0; i < items->len; i++) {
    Item *item = g_ptr_array_index(items, i);
    if (matches(item, query))
      return g_strdup(item->password);
  }
  return NULL;
}

gboolean __wrap_secret_password_clear_sync(const SecretSchema *schema,
                                           GCancellable *cancel, GError **error,
                                           ...) {
  (void)schema;
  if (!check(cancel, error))
    return FALSE;
  va_list args;
  va_start(args, error);
  g_autoptr(GHashTable) query = attributes(args);
  va_end(args);
  gboolean removed = FALSE;
  for (guint i = 0; i < items->len;) {
    if (matches(g_ptr_array_index(items, i), query)) {
      g_ptr_array_remove_index(items, i);
      removed = TRUE;
    } else
      i++;
  }
  return removed;
}
