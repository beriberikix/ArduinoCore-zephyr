/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Golioth.h"

#include <stdio.h>
#include <string.h>

/* Golioth's stream service lives under ".s/". Accept a bare leaf name for
 * ergonomics, and let a caller who spells out a service prefix through. */
static const char *qualify(const char *path, char *buf, size_t buflen)
{
	if (path == nullptr) {
		return nullptr;
	}

	if (path[0] == '.' && path[1] != '\0' && path[2] == '/') {
		return path;
	}

	while (*path == '/') {
		path++;
	}

	snprintf(buf, buflen, ".s/%s", path);
	return buf;
}

int GoliothClass::begin()
{
	return arduino_pouch_begin();
}

int GoliothClass::begin(const unsigned char *crt, size_t crtLen,
			const unsigned char *key, size_t keyLen)
{
	int err = arduino_pouch_set_credentials(crt, crtLen, key, keyLen);
	if (err) {
		return err;
	}

	return arduino_pouch_begin();
}

int GoliothClass::streamRaw(const char *path, const void *data, size_t len, uint16_t contentType)
{
	char buf[64];
	const char *full = qualify(path, buf, sizeof(buf));

	if (full == nullptr) {
		return -EINVAL;
	}

	return arduino_pouch_stream(full, data, len, contentType);
}

int GoliothClass::stream(const char *path, const char *json)
{
	if (json == nullptr) {
		return -EINVAL;
	}

	return streamRaw(path, json, strlen(json), GOLIOTH_JSON);
}

int GoliothClass::stream(const char *path, float value)
{
	char json[40];
	int n = snprintf(json, sizeof(json), "{\"value\":%.3f}", (double) value);

	return streamRaw(path, json, n, GOLIOTH_JSON);
}

int GoliothClass::stream(const char *path, int value)
{
	char json[32];
	int n = snprintf(json, sizeof(json), "{\"value\":%d}", value);

	return streamRaw(path, json, n, GOLIOTH_JSON);
}

/*
 * Minimal CBOR encoder for {"level": <level>, "message": <message>}. Hand-rolled
 * rather than pulling zcbor into the sketch: two text keys and two text values
 * is the whole grammar we need, and it keeps the llext free of another
 * dependency. Strings longer than 255 bytes are truncated.
 */
static size_t cbor_text(uint8_t *out, const char *s, size_t max)
{
	size_t len = strlen(s);
	size_t i = 0;

	if (len > max) {
		len = max;
	}

	if (len < 24) {
		out[i++] = 0x60 | (uint8_t) len;      /* text, immediate length */
	} else {
		out[i++] = 0x78;                     /* text, 1-byte length */
		out[i++] = (uint8_t) len;
	}

	memcpy(&out[i], s, len);

	return i + len;
}

int GoliothClass::log(const char *level, const char *message)
{
	if (level == nullptr || message == nullptr) {
		return -EINVAL;
	}

	uint8_t cbor[288];
	size_t i = 0;

	cbor[i++] = 0xa2;                                        /* map(2) */
	i += cbor_text(&cbor[i], "level", 8);
	i += cbor_text(&cbor[i], level, 16);
	i += cbor_text(&cbor[i], "message", 8);
	i += cbor_text(&cbor[i], message, 200);

	return streamRaw("logs", cbor, i, GOLIOTH_CBOR);
}

GoliothClass Golioth;
