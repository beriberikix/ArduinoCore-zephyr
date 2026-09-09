/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The loader half of the Golioth Pouch bridge.
 *
 * Pouch registers uplink, event, downlink, Settings and OTA handlers through
 * linker iterable sections, which an llext cannot contribute to. So the loader
 * owns every section entry and re-exports the runtime registration functions
 * below; libraries/Golioth declares the same prototypes by hand on the sketch
 * side (house style - no library in this core includes a loader header).
 *
 * Keep every signature free of Pouch and PSA types: sketches must not need
 * <pouch/...> to use this, and Pouch's API churn has to stop at this boundary.
 * That is also why the event argument is a plain int rather than
 * enum pouch_event.
 */

/** Mirrors enum pouch_event. Kept in sync by hand, deliberately. */
#define ARDUINO_POUCH_EVENT_SESSION_START 0
#define ARDUINO_POUCH_EVENT_SESSION_END   1

typedef void (*arduino_pouch_event_cb_t)(int event, void *user_data);

typedef void (*arduino_pouch_downlink_start_cb_t)(unsigned int stream_id, const char *path,
						  uint16_t content_type, void *user_data);
typedef void (*arduino_pouch_downlink_data_cb_t)(unsigned int stream_id, const void *data,
						 size_t len, int is_last, void *user_data);

/**
 * Register a session lifecycle callback. Pass NULL to clear.
 *
 * Runs on Pouch's work queue thread, not the sketch's - keep it short and do
 * not block. Register before the session opens where possible:
 * CONFIG_ARDUINO_POUCH_AUTOSTART starts the session before setup() runs, so an
 * early SESSION_START can be missed.
 */
int arduino_pouch_on_event(arduino_pouch_event_cb_t cb, void *user_data);

/**
 * Register downlink callbacks. Either may be NULL; passing NULL for both clears
 * the registration.
 *
 * Registering also forces the sync loop to keep syncing when there is nothing
 * to send, because downlink only arrives as the response to a sync.
 */
int arduino_pouch_on_downlink(arduino_pouch_downlink_start_cb_t start_cb,
			      arduino_pouch_downlink_data_cb_t data_cb, void *user_data);

/*
 * Golioth Settings.
 *
 * GOLIOTH_SETTINGS_HANDLER() fixes both the key and the value type at compile
 * time, so the loader keeps a small pool of slots per type whose keys live in
 * RAM, and binding a key to a callback is what claims one. See
 * loader/pouch_services.c.
 *
 * Callbacks return 0 to accept the value, or a negative errno to reject it.
 * They run on Pouch's work queue thread. Pass cb = NULL to release the slot.
 * There are CONFIG_ARDUINO_POUCH_SETTING_SLOTS slots per type; keys are capped
 * at 31 characters, matching Golioth's own limit.
 */
typedef int (*arduino_pouch_setting_int_cb_t)(int32_t value, void *user_data);
typedef int (*arduino_pouch_setting_bool_cb_t)(bool value, void *user_data);
typedef int (*arduino_pouch_setting_float_cb_t)(double value, void *user_data);
typedef int (*arduino_pouch_setting_string_cb_t)(const char *value, size_t len, void *user_data);

int arduino_pouch_on_setting_int(const char *key, arduino_pouch_setting_int_cb_t cb,
				 void *user_data);
int arduino_pouch_on_setting_bool(const char *key, arduino_pouch_setting_bool_cb_t cb,
				  void *user_data);
int arduino_pouch_on_setting_float(const char *key, arduino_pouch_setting_float_cb_t cb,
				   void *user_data);
int arduino_pouch_on_setting_string(const char *key, arduino_pouch_setting_string_cb_t cb,
				    void *user_data);

/*
 * Golioth OTA.
 *
 * Two components are registered by the loader: the MCUboot image it is itself
 * built as, and the llext sketch in user_sketch. Package names come from
 * CONFIG_ARDUINO_POUCH_OTA_FW_PACKAGE / _SKETCH_PACKAGE.
 *
 * By default the loader downloads any component whose target differs from its
 * current version, writes it and reboots, with no sketch involvement at all.
 * Registering a manifest callback takes that decision over completely: nothing
 * is downloaded until the sketch calls arduino_pouch_ota_mark().
 */
#define ARDUINO_POUCH_OTA_IDLE     0
#define ARDUINO_POUCH_OTA_DOWNLOAD 1

typedef void (*arduino_pouch_ota_manifest_cb_t)(const char *package, const char *current,
						const char *target, size_t size, void *user_data);

int arduino_pouch_on_ota_manifest(arduino_pouch_ota_manifest_cb_t cb, void *user_data);
int arduino_pouch_ota_mark(const char *package, int state);

/**
 * Declare the running sketch's version, as reported to Golioth for the sketch
 * component. Until a sketch calls this the device reports "0.0.0", so the cloud
 * will offer any published sketch artifact. Call it in setup().
 */
int arduino_pouch_ota_sketch_version(const char *version);
