/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Golioth OTA, for both halves of what runs on this board.
 *
 * Pouch's OTA service is bootloader-agnostic - golioth_sdk/ has no flash code at
 * all. A component is just a named package plus a receive(data, offset, len,
 * is_last) callback, so the two things this device is made of can each be one:
 *
 *   "loader"  a signed MCUboot image -> staged in slot1, swapped on reboot
 *   "sketch"  a raw .ino.elf-zsk.bin -> written straight into user_sketch
 *
 * They are genuinely different update mechanisms (MCUboot swap vs. llext
 * reload), which is why the sketch partition had to be moved out of slot1: a
 * swap would otherwise destroy the sketch. See the variant overlay's flash map.
 *
 * Only one GOLIOTH_OTA_MANIFEST_HANDLER may exist per image - the macro
 * declares a non-static symbol at a fixed name - so the single handler below
 * dispatches for both components.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ARDUINO_POUCH) && defined(CONFIG_GOLIOTH_OTA)

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(arduino_pouch_ota, CONFIG_ARDUINO_POUCH_LOG_LEVEL);

#include <errno.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include <golioth/ota.h>

#include "arduino_pouch.h"

/* Mirrors enum golioth_ota_state (golioth_sdk/ota.h), which is not public. */
#define ARDUINO_OTA_STATE_IDLE        0
#define ARDUINO_OTA_STATE_DOWNLOADING 1

static arduino_pouch_ota_manifest_cb_t sketch_manifest_cb;
static void *sketch_manifest_ctx;

/*
 * A sketch's version is not knowable when this loader is compiled, and the zsk
 * header carries no semantic version. .version is a const char * and nothing
 * requires it to be a literal, so it points at RAM and a sketch declares its
 * own version through arduino_pouch_ota_sketch_version(). Until it does, the
 * device reports 0.0.0 and the cloud will offer any published sketch artifact.
 */
static char sketch_version[CONFIG_GOLIOTH_OTA_MAX_VERSION_LEN] = "0.0.0";

/* ------------------------------------------------------------------ loader */

static struct flash_img_context fw_ctx;
static bool fw_ctx_ready;

static void ota_fw_receive(const void *data, size_t offset, size_t len, bool is_last)
{
	int err;

	if (offset == 0) {
		err = flash_img_init(&fw_ctx);
		if (err) {
			LOG_ERR("flash_img_init failed: %d", err);
			fw_ctx_ready = false;
			return;
		}
		fw_ctx_ready = true;
		LOG_INF("Loader update starting");
	}

	if (!fw_ctx_ready) {
		return;
	}

	err = flash_img_buffered_write(&fw_ctx, (uint8_t *) data, len, is_last);
	if (err) {
		LOG_ERR("Write at offset %zu failed: %d", offset, err);
		fw_ctx_ready = false;
		return;
	}

	if (!is_last) {
		return;
	}

	fw_ctx_ready = false;

	/*
	 * MCUboot verifies the image signature on the next boot; nothing here
	 * checks the manifest's target_hash, exactly as pouch's own example does.
	 */
	err = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
	if (err) {
		LOG_ERR("boot_request_upgrade failed: %d", err);
		return;
	}

	LOG_INF("Loader image staged, rebooting to swap");
	k_sleep(K_SECONDS(2));
	sys_reboot(SYS_REBOOT_WARM);
}

/* ------------------------------------------------------------------ sketch */

#define SKETCH_PARTITION  FIXED_PARTITION_ID(user_sketch)
#define SKETCH_SIZE       FIXED_PARTITION_SIZE(user_sketch)

static const struct flash_area *sketch_fa;

static void ota_sketch_receive(const void *data, size_t offset, size_t len, bool is_last)
{
	int err;

	if (offset == 0) {
		err = flash_area_open(SKETCH_PARTITION, &sketch_fa);
		if (err) {
			LOG_ERR("Cannot open user_sketch: %d", err);
			sketch_fa = NULL;
			return;
		}

		/*
		 * Erase the whole partition up front rather than progressively:
		 * a partial write must not leave a sketch whose zsk header still
		 * validates, or the loader would happily run a truncated image.
		 */
		LOG_INF("Erasing user_sketch (%u bytes)", (unsigned int) SKETCH_SIZE);
		err = flash_area_erase(sketch_fa, 0, SKETCH_SIZE);
		if (err) {
			LOG_ERR("Erase failed: %d", err);
			flash_area_close(sketch_fa);
			sketch_fa = NULL;
			return;
		}
	}

	if (sketch_fa == NULL) {
		return;
	}

	if (offset + len > SKETCH_SIZE) {
		LOG_ERR("Sketch image overruns partition (%zu > %u)", offset + len,
			(unsigned int) SKETCH_SIZE);
		flash_area_close(sketch_fa);
		sketch_fa = NULL;
		return;
	}

	err = flash_area_write(sketch_fa, offset, data, len);
	if (err) {
		LOG_ERR("Write at offset %zu failed: %d", offset, err);
		flash_area_close(sketch_fa);
		sketch_fa = NULL;
		return;
	}

	if (!is_last) {
		return;
	}

	/*
	 * Same check loader() makes before llext_load(): a 16-byte "zsk" header
	 * with ver 0x1 at +0x07 and magic 0x2341 at +0x0c. Catching it here means
	 * a bad artifact is rejected before the reboot, not after.
	 */
	uint8_t header[16];

	err = flash_area_read(sketch_fa, 0, header, sizeof(header));
	flash_area_close(sketch_fa);
	sketch_fa = NULL;

	if (err) {
		LOG_ERR("Header read-back failed: %d", err);
		return;
	}

	uint8_t ver = header[0x07];
	uint16_t magic = sys_get_le16(&header[0x0c]);

	if (ver != 0x1 || magic != 0x2341) {
		LOG_ERR("Downloaded sketch has no valid zsk header (ver %u magic 0x%04x)", ver,
			magic);
		return;
	}

	LOG_INF("Sketch written, rebooting to load it");
	k_sleep(K_SECONDS(2));
	sys_reboot(SYS_REBOOT_WARM);
}

/* ---------------------------------------------------------------- manifest */

static void ota_manifest_receive(const struct golioth_ota_manifest_component *components,
				 size_t num_components)
{
	if (sketch_manifest_cb != NULL) {
		/*
		 * A sketch that registers takes over the download decision
		 * entirely - it can gate on battery, user confirmation, whatever -
		 * and drives it with arduino_pouch_ota_mark().
		 */
		for (size_t i = 0; i < num_components; i++) {
			sketch_manifest_cb(components[i].name, components[i].current,
					   components[i].target, components[i].size,
					   sketch_manifest_ctx);
		}
		return;
	}

	for (size_t i = 0; i < num_components; i++) {
		if (components[i].current == NULL || components[i].target == NULL) {
			continue;
		}

		if (strcmp(components[i].current, components[i].target) == 0) {
			continue;
		}

		LOG_INF("Component '%s': %s -> %s (%zu bytes)", components[i].name,
			components[i].current, components[i].target, components[i].size);

		int err = golioth_ota_mark_for_download(components[i].name);
		if (err) {
			LOG_ERR("mark_for_download('%s') failed: %d", components[i].name, err);
		}
	}
}

GOLIOTH_OTA_COMPONENT(fw, CONFIG_ARDUINO_POUCH_OTA_FW_PACKAGE, APP_VERSION_STRING, ota_fw_receive);

/*
 * The sketch component cannot use GOLIOTH_OTA_COMPONENT(): that macro puts
 * _version into both .version (a const char *, which may point at RAM) and
 * .target (a char array, which needs a literal initialiser). A sketch's version
 * is only known at runtime, so the macro is expanded by hand here with .target
 * seeded to the same default sketch_version holds, and
 * arduino_pouch_ota_sketch_version() updates both.
 */
struct golioth_ota_registered_component_data ota_component_data_sketch = {
	.target = "0.0.0",
	.state = 0,
};
const POUCH_STRUCT_SECTION_ITERABLE(golioth_ota_registered_component, ota_component_sketch) = {
	.name = CONFIG_ARDUINO_POUCH_OTA_SKETCH_PACKAGE,
	.version = sketch_version,
	.receive = ota_sketch_receive,
	.data = &ota_component_data_sketch,
};

GOLIOTH_OTA_MANIFEST_HANDLER(ota_manifest_receive);

/* ------------------------------------------------------------ sketch-facing */

int arduino_pouch_on_ota_manifest(arduino_pouch_ota_manifest_cb_t cb, void *user_data)
{
	sketch_manifest_ctx = user_data;
	compiler_barrier();
	sketch_manifest_cb = cb;

	return 0;
}

int arduino_pouch_ota_mark(const char *package, int state)
{
	if (package == NULL) {
		return -EINVAL;
	}

	switch (state) {
	case ARDUINO_OTA_STATE_DOWNLOADING:
		return golioth_ota_mark_for_download(package);
	case ARDUINO_OTA_STATE_IDLE:
		return golioth_ota_mark_idle(package);
	default:
		return -EINVAL;
	}
}

int arduino_pouch_ota_sketch_version(const char *version)
{
	if (version == NULL || version[0] == '\0') {
		return -EINVAL;
	}

	if (strlen(version) >= sizeof(sketch_version)) {
		return -ENAMETOOLONG;
	}

	strncpy(sketch_version, version, sizeof(sketch_version) - 1);
	sketch_version[sizeof(sketch_version) - 1] = '\0';

	/*
	 * .target seeds what gets reported until a manifest arrives; keep it in
	 * step so the first status uplink does not advertise 0.0.0.
	 */
	strncpy(ota_component_data_sketch.target, sketch_version,
		sizeof(ota_component_data_sketch.target) - 1);
	ota_component_data_sketch.target[sizeof(ota_component_data_sketch.target) - 1] = '\0';

	LOG_INF("Sketch version reported as %s", sketch_version);

	return 0;
}

#endif /* CONFIG_ARDUINO_POUCH && CONFIG_GOLIOTH_OTA */
