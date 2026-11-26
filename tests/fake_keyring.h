#ifndef TEST_FAKE_KEYRING_H
#define TEST_FAKE_KEYRING_H
#include <glib.h>
void fake_keyring_reset(gboolean available);
guint fake_keyring_count(void);
#endif
