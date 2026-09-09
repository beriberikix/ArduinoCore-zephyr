/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Golioth services that a sketch cannot register for itself: Settings, and
 * later OTA. Both are driven by linker iterable sections, so the loader owns
 * the section entries and re-exports runtime registration.
 *
 * Settings is the awkward one. GOLIOTH_SETTINGS_HANDLER() bakes in both the key
 * (as a string literal, .key = #_name) and the value type (via _Generic on the
 * callback), so there is no way to register "whatever key the sketch asks for"
 * from an llext.
 *
 * The trick used here: a section entry's .key is a `const char *`, and nothing
 * says it has to point at a literal. Each slot below points its .key at a
 * mutable RAM buffer that starts empty. golioth_settings_receive_one() matches
 * with strcmp(), so an empty slot simply never matches, and settings_uplink()
 * reports only {"version": N} - it does not enumerate handlers - so unused
 * slots are invisible to the cloud. Registering a setting copies the key into
 * that buffer.
 *
 * The type still has to be fixed per slot, hence one pool per type.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ARDUINO_POUCH) && defined(CONFIG_GOLIOTH_SETTINGS)

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(arduino_pouch_svc, CONFIG_ARDUINO_POUCH_LOG_LEVEL);

#include <errno.h>
#include <string.h>

#include <golioth/settings_callbacks.h>
#include <golioth/settings_types.h>
#include <pouch/port.h>

#include "arduino_pouch.h"

/* Same cap settings.c applies when decoding a key (golioth_sdk/settings.c:25). */
#define ARDUINO_SETTING_KEY_MAX 31

#define SLOTS CONFIG_ARDUINO_POUCH_SETTING_SLOTS

/*
 * One pool per value type. LIST(n) expands the pool; the trampoline reads the
 * sketch pointer under a NULL guard because a settings downlink can land before
 * setup() has registered anything.
 */
#define ARDUINO_SETTING_POOL(kind, ctype, cbtype)                                                  \
	static char kind##_key[SLOTS][ARDUINO_SETTING_KEY_MAX + 1];                                \
	static cbtype kind##_cb[SLOTS];                                                            \
	static void *kind##_ctx[SLOTS];                                                            \
	static int kind##_dispatch(unsigned int slot, ctype value)                                 \
	{                                                                                          \
		if (kind##_cb[slot] == NULL) {                                                     \
			return -ENOENT;                                                            \
		}                                                                                  \
		return kind##_cb[slot](value, kind##_ctx[slot]);                                   \
	}

#define ARDUINO_SETTING_SLOT(kind, KIND, ctype, n)                                                 \
	static int kind##_tramp_##n(ctype value)                                                   \
	{                                                                                          \
		return kind##_dispatch(n, value);                                                  \
	}                                                                                          \
	static const POUCH_STRUCT_SECTION_ITERABLE(golioth_settings_handler, kind##_slot_##n) = {  \
		.key = kind##_key[n],                                                              \
		.type = GOLIOTH_SETTING_VALUE_TYPE_##KIND,                                         \
		/* Same union aliasing GOLIOTH_SETTINGS_HANDLER() itself uses. */                  \
		.generic = (void *) kind##_tramp_##n,                                              \
	}

ARDUINO_SETTING_POOL(sint, int32_t, arduino_pouch_setting_int_cb_t)
ARDUINO_SETTING_POOL(sbool, bool, arduino_pouch_setting_bool_cb_t)
ARDUINO_SETTING_POOL(sfloat, double, arduino_pouch_setting_float_cb_t)

/* Strings carry a length, so they do not fit the scalar pool shape. */
static char sstr_key[SLOTS][ARDUINO_SETTING_KEY_MAX + 1];
static arduino_pouch_setting_string_cb_t sstr_cb[SLOTS];
static void *sstr_ctx[SLOTS];

static int sstr_dispatch(unsigned int slot, const char *value, size_t len)
{
	if (sstr_cb[slot] == NULL) {
		return -ENOENT;
	}
	return sstr_cb[slot](value, len, sstr_ctx[slot]);
}

#define ARDUINO_SETTING_STR_SLOT(n)                                                                \
	static int sstr_tramp_##n(const char *value, size_t len)                                   \
	{                                                                                          \
		return sstr_dispatch(n, value, len);                                               \
	}                                                                                          \
	static const POUCH_STRUCT_SECTION_ITERABLE(golioth_settings_handler, sstr_slot_##n) = {    \
		.key = sstr_key[n],                                                                \
		.type = GOLIOTH_SETTING_VALUE_TYPE_STRING,                                         \
		.generic = (void *) sstr_tramp_##n,                                                \
	}

/*
 * The section entries have to exist at link time, so the pool size is a
 * compile-time list. Four of each is enough for anything a sketch is likely to
 * drive by hand; raise CONFIG_ARDUINO_POUCH_SETTING_SLOTS and extend here.
 */
#if SLOTS > 4
#error "Add matching ARDUINO_SETTING_SLOT() lines when raising CONFIG_ARDUINO_POUCH_SETTING_SLOTS"
#endif

ARDUINO_SETTING_SLOT(sint, INT, int32_t, 0);
ARDUINO_SETTING_SLOT(sbool, BOOL, bool, 0);
ARDUINO_SETTING_SLOT(sfloat, FLOAT, double, 0);
ARDUINO_SETTING_STR_SLOT(0);
#if SLOTS > 1
ARDUINO_SETTING_SLOT(sint, INT, int32_t, 1);
ARDUINO_SETTING_SLOT(sbool, BOOL, bool, 1);
ARDUINO_SETTING_SLOT(sfloat, FLOAT, double, 1);
ARDUINO_SETTING_STR_SLOT(1);
#endif
#if SLOTS > 2
ARDUINO_SETTING_SLOT(sint, INT, int32_t, 2);
ARDUINO_SETTING_SLOT(sbool, BOOL, bool, 2);
ARDUINO_SETTING_SLOT(sfloat, FLOAT, double, 2);
ARDUINO_SETTING_STR_SLOT(2);
#endif
#if SLOTS > 3
ARDUINO_SETTING_SLOT(sint, INT, int32_t, 3);
ARDUINO_SETTING_SLOT(sbool, BOOL, bool, 3);
ARDUINO_SETTING_SLOT(sfloat, FLOAT, double, 3);
ARDUINO_SETTING_STR_SLOT(3);
#endif

/*
 * Claim a slot for 'key': the one already bound to it if there is one (so
 * re-registering replaces rather than exhausts the pool), otherwise the first
 * free one. Returns the index, or -errno.
 */
static int slot_claim(char keys[][ARDUINO_SETTING_KEY_MAX + 1], const char *key)
{
	int free_slot = -1;

	if (key == NULL || key[0] == '\0') {
		return -EINVAL;
	}
	if (strlen(key) > ARDUINO_SETTING_KEY_MAX) {
		LOG_ERR("Setting key '%s' longer than %d", key, ARDUINO_SETTING_KEY_MAX);
		return -ENAMETOOLONG;
	}

	for (int i = 0; i < SLOTS; i++) {
		if (strcmp(keys[i], key) == 0) {
			return i;
		}
		if (free_slot < 0 && keys[i][0] == '\0') {
			free_slot = i;
		}
	}

	if (free_slot < 0) {
		LOG_ERR("No free settings slot for '%s' (%d per type)", key, SLOTS);
		return -ENOSPC;
	}

	return free_slot;
}

/*
 * Publish order in each of these: callback and context first, key last. The key
 * is what golioth_settings_receive_one() matches on, so writing it last means a
 * downlink arriving mid-registration either misses the slot entirely or finds
 * it fully built - never a new key against a stale callback.
 */
#define ARDUINO_SETTING_REGISTER(name, kind, cbtype)                                               \
	int name(const char *key, cbtype cb, void *user_data)                                      \
	{                                                                                          \
		int slot = slot_claim(kind##_key, key);                                            \
		if (slot < 0) {                                                                    \
			return slot;                                                               \
		}                                                                                  \
		kind##_cb[slot] = cb;                                                              \
		kind##_ctx[slot] = user_data;                                                      \
		compiler_barrier();                                                                \
		if (cb == NULL) {                                                                  \
			kind##_key[slot][0] = '\0';                                                \
		} else {                                                                           \
			strncpy(kind##_key[slot], key, ARDUINO_SETTING_KEY_MAX);                   \
			kind##_key[slot][ARDUINO_SETTING_KEY_MAX] = '\0';                          \
		}                                                                                  \
		LOG_DBG("Setting '%s' -> %s slot %d", key, #kind, slot);                           \
		return 0;                                                                          \
	}

ARDUINO_SETTING_REGISTER(arduino_pouch_on_setting_int, sint, arduino_pouch_setting_int_cb_t)
ARDUINO_SETTING_REGISTER(arduino_pouch_on_setting_bool, sbool, arduino_pouch_setting_bool_cb_t)
ARDUINO_SETTING_REGISTER(arduino_pouch_on_setting_float, sfloat, arduino_pouch_setting_float_cb_t)
ARDUINO_SETTING_REGISTER(arduino_pouch_on_setting_string, sstr, arduino_pouch_setting_string_cb_t)

#endif /* CONFIG_ARDUINO_POUCH && CONFIG_GOLIOTH_SETTINGS */
