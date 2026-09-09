/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/*
 * autoconf.h is -imacros'd into every sketch translation unit (platform.txt),
 * so the loader's configuration is visible here. Failing at compile time beats
 * failing at llext_load with an undefined symbol.
 */
#if !defined(CONFIG_ARDUINO_POUCH)
#error "This board's loader was not built with CONFIG_ARDUINO_POUCH=y"
#endif

/*
 * The Pouch session is owned by the loader, not by the sketch: Pouch registers
 * its uplink and event handlers through linker iterable sections, which an
 * llext cannot contribute to. The five symbols below are loader-owned shims
 * exported from llext_exports.c.
 *
 * They are declared here rather than pulled in from <pouch/...> on purpose, so
 * a sketch needs no Pouch or PSA headers and is unaffected by Pouch's API
 * churn. The loader must be built with CONFIG_ARDUINO_POUCH=y.
 */
extern "C" {
int arduino_pouch_begin(void);
int arduino_pouch_set_credentials(const unsigned char *crt, size_t crt_len,
                                  const unsigned char *key, size_t key_len);
int arduino_pouch_stream(const char *path, const void *data, size_t len, uint16_t content_type);
int arduino_pouch_status(void);
int arduino_pouch_sync_now(uint32_t timeout_ms);

/* Registration shims. The loader owns every linker-section entry and calls back
 * into the sketch through these; see loader/arduino_pouch.h. */
int arduino_pouch_on_event(void (*cb)(int event, void *user_data), void *user_data);
int arduino_pouch_on_downlink(void (*start_cb)(unsigned int stream_id, const char *path,
					       uint16_t content_type, void *user_data),
			      void (*data_cb)(unsigned int stream_id, const void *data, size_t len,
					      int is_last, void *user_data),
			      void *user_data);
#if defined(CONFIG_GOLIOTH_SETTINGS)
int arduino_pouch_on_setting_int(const char *key, int (*cb)(int32_t, void *), void *user_data);
int arduino_pouch_on_setting_bool(const char *key, int (*cb)(bool, void *), void *user_data);
int arduino_pouch_on_setting_float(const char *key, int (*cb)(double, void *), void *user_data);
int arduino_pouch_on_setting_string(const char *key, int (*cb)(const char *, size_t, void *),
				    void *user_data);
#endif
#if defined(CONFIG_GOLIOTH_OTA)
int arduino_pouch_on_ota_manifest(void (*cb)(const char *package, const char *current,
					     const char *target, size_t size, void *user_data),
				  void *user_data);
int arduino_pouch_ota_mark(const char *package, int state);
int arduino_pouch_ota_sketch_version(const char *version);
#endif
}

/** Values returned by Golioth.status(). Negative means -errno from the last sync. */
enum GoliothState {
	GOLIOTH_IDLE       = 0,
	GOLIOTH_CONNECTING = 1,
	GOLIOTH_ONLINE     = 2,
};

/** Values passed to a Golioth.onSession() callback. */
enum GoliothSessionEvent {
	GOLIOTH_SESSION_START = 0,
	GOLIOTH_SESSION_END   = 1,
};

/** OTA component states accepted by arduino_pouch_ota_mark(). */
#define ARDUINO_POUCH_OTA_IDLE     0
#define ARDUINO_POUCH_OTA_DOWNLOAD 1

/** Pouch content types, from the IANA CoRE Content-Formats registry. */
enum GoliothContentType {
	GOLIOTH_OCTET_STREAM = 42,
	GOLIOTH_JSON         = 50,
	GOLIOTH_CBOR         = 60,
};

class GoliothClass {
public:
	/**
	 * Start the Pouch session using the certificate compiled into the loader.
	 * Returns immediately; poll connected() or status(). Bring the network up
	 * first (e.g. WiFi.begin()) or the loader will do it from its own
	 * compiled-in credentials.
	 */
	int begin();

	/** As begin(), but with a certificate and key supplied by the sketch.
	 *  Both buffers must stay valid for as long as the session runs. */
	int begin(const unsigned char *crt, size_t crtLen,
		  const unsigned char *key, size_t keyLen);

	/** GOLIOTH_IDLE / GOLIOTH_CONNECTING / GOLIOTH_ONLINE, or -errno. */
	int status() { return arduino_pouch_status(); }
	bool connected() { return arduino_pouch_status() == GOLIOTH_ONLINE; }

	/** Stream a value. A bare path is sent under Golioth's stream service,
	 *  so "sensor" becomes ".s/sensor". Encoded as {"value": <v>}. */
	int stream(const char *path, float value);
	int stream(const char *path, int value);

	/** Stream a JSON document verbatim. The caller owns the encoding. */
	int stream(const char *path, const char *json);

	/** Stream pre-encoded bytes with an explicit content type. */
	int streamRaw(const char *path, const void *data, size_t len, uint16_t contentType);

	/**
	 * Send a log line to the Golioth Logs service.
	 *
	 * Pouch has no logging API: Golioth routes logs through Pipelines, so a
	 * log entry is a CBOR {"level","message"} map streamed to ".s/logs",
	 * which the default CBOR logs Pipeline picks up.
	 */
	int log(const char *message) { return log("info", message); }
	int log(const char *level, const char *message);

	/** Force a sync now. 0 returns immediately, otherwise wait up to timeoutMs. */
	int sync(uint32_t timeoutMs = 0) { return arduino_pouch_sync_now(timeoutMs); }

	/*
	 * Callbacks below all run on the loader's Pouch thread, never in ISR
	 * context and never on the sketch's own thread. Keep them short and do
	 * not block: the Pouch session is waiting on them. Pass nullptr to clear
	 * a registration, as libraries/CAN does.
	 */

	typedef void (*SessionCallback)(int event, void *userData);
	typedef void (*DownlinkStartCallback)(unsigned int streamId, const char *path,
					      uint16_t contentType, void *userData);
	typedef void (*DownlinkDataCallback)(unsigned int streamId, const void *data, size_t len,
					     int isLast, void *userData);

	/** GOLIOTH_SESSION_START / GOLIOTH_SESSION_END.
	 *  Register in setup(): the loader may open a session before then. */
	int onSession(SessionCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_event(cb, userData);
	}

	/**
	 * Receive downlink payloads. Registering also stops the loader skipping
	 * syncs when the sketch has nothing to send - downlink only ever arrives
	 * as the answer to a sync.
	 */
	int onDownlink(DownlinkStartCallback startCb, DownlinkDataCallback dataCb,
		       void *userData = nullptr)
	{
		return arduino_pouch_on_downlink(startCb, dataCb, userData);
	}

#if defined(CONFIG_GOLIOTH_SETTINGS)
	/*
	 * Golioth Settings. The key must match the setting's name in the Golioth
	 * console exactly, and its type must match the overload used, or the
	 * value is rejected with -EINVAL.
	 *
	 * Return 0 from a callback to accept, negative errno to reject. There are
	 * CONFIG_ARDUINO_POUCH_SETTING_SLOTS slots per type (4 by default);
	 * -ENOSPC means the pool for that type is full.
	 */
	typedef int (*IntSettingCallback)(int32_t value, void *userData);
	typedef int (*BoolSettingCallback)(bool value, void *userData);
	typedef int (*FloatSettingCallback)(double value, void *userData);
	typedef int (*StringSettingCallback)(const char *value, size_t len, void *userData);

	int onSetting(const char *key, IntSettingCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_setting_int(key, cb, userData);
	}
	int onSetting(const char *key, BoolSettingCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_setting_bool(key, cb, userData);
	}
	int onSetting(const char *key, FloatSettingCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_setting_float(key, cb, userData);
	}
	int onSetting(const char *key, StringSettingCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_setting_string(key, cb, userData);
	}
#endif

#if defined(CONFIG_GOLIOTH_OTA)
	/*
	 * Golioth OTA. Two packages are published by this board: the loader (a
	 * signed MCUboot image, swapped on reboot) and the sketch (a raw llext
	 * written into user_sketch). Both reboot the board when complete.
	 *
	 * Doing nothing here is a valid choice - the loader downloads and applies
	 * any newer component on its own. Registering a manifest callback takes
	 * that over: nothing downloads until you call otaDownload().
	 */
	typedef void (*ManifestCallback)(const char *package, const char *current,
					 const char *target, size_t size, void *userData);

	int onFirmwareManifest(ManifestCallback cb, void *userData = nullptr)
	{
		return arduino_pouch_on_ota_manifest(cb, userData);
	}

	int otaDownload(const char *package)
	{
		return arduino_pouch_ota_mark(package, ARDUINO_POUCH_OTA_DOWNLOAD);
	}
	int otaIdle(const char *package)
	{
		return arduino_pouch_ota_mark(package, ARDUINO_POUCH_OTA_IDLE);
	}

	/** Report this sketch's version to Golioth. Without it the device reports
	 *  "0.0.0" and the cloud offers any published sketch artifact. */
	int version(const char *v) { return arduino_pouch_ota_sketch_version(v); }
#endif
};

extern GoliothClass Golioth;
