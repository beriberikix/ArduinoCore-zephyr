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
}

/** Values returned by Golioth.status(). Negative means -errno from the last sync. */
enum GoliothState {
	GOLIOTH_IDLE       = 0,
	GOLIOTH_CONNECTING = 1,
	GOLIOTH_ONLINE     = 2,
};

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
};

extern GoliothClass Golioth;
