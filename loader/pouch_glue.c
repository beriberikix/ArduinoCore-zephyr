/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Loader-side glue for the Golioth Pouch protocol.
 *
 * Pouch registers uplink and event handlers through linker iterable sections
 * (POUCH_UPLINK_HANDLER, POUCH_EVENT_HANDLER). An llext cannot contribute to
 * the loader's iterable sections, so a sketch can never register one, and the
 * Pouch session has to be owned here.
 *
 * What a sketch gets instead is the five arduino_pouch_* functions at the
 * bottom of this file, exported through llext_exports.c. They pass only
 * scalars and pointers, so sketches need no Pouch or PSA headers, and Pouch's
 * API churn stops at this boundary.
 *
 * Entries a sketch writes between syncs are not lost: pouch_uplink_entry_write()
 * fills a block and queues it, and pouch_uplink_start() drains that queue when
 * the next session opens (pouch/src/uplink.c).
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ARDUINO_POUCH)

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(arduino_pouch, CONFIG_ARDUINO_POUCH_LOG_LEVEL);

#include <errno.h>
#include <string.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/dhcpv4.h>
#if defined(CONFIG_WIFI)
#include <zephyr/net/wifi_mgmt.h>
#endif

#include <mbedtls/pk.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>

#include <pouch/pouch.h>
#include <pouch/uplink.h>
#include <pouch/downlink.h>
#include <pouch/events.h>
#include <pouch/types.h>
#if defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
#include <pouch/transport/coap/client.h>
#endif

#include "arduino_pouch.h"
#include "pouch_credentials.h"

#if defined(CONFIG_ARDUINO_POUCH_HEARTBEAT_LED)
#include <zephyr/drivers/gpio.h>

/*
 * Boards whose console is on physical UART pins give no sign of life over USB.
 * Drive the RGB LED as a coarse progress indicator so the stage a hang happens
 * in can be read off the board:
 *
 *   red    - thread running, about to initialise Pouch
 *   yellow - Pouch initialised (certificate parsed, PSA key imported)
 *   green  - transport up; blinking once the device is advertising
 */
static const struct gpio_dt_spec hb_r = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static const struct gpio_dt_spec hb_g = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0});
static const struct gpio_dt_spec hb_b = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led2), gpios, {0});

static void hb_init(void)
{
	if (hb_r.port) gpio_pin_configure_dt(&hb_r, GPIO_OUTPUT_INACTIVE);
	if (hb_g.port) gpio_pin_configure_dt(&hb_g, GPIO_OUTPUT_INACTIVE);
	if (hb_b.port) gpio_pin_configure_dt(&hb_b, GPIO_OUTPUT_INACTIVE);
}

static void hb_set(int r, int g, int b)
{
	if (hb_r.port) gpio_pin_set_dt(&hb_r, r);
	if (hb_g.port) gpio_pin_set_dt(&hb_g, g);
	if (hb_b.port) gpio_pin_set_dt(&hb_b, b);
}
#else
#define hb_init()          do { } while (0)
#define hb_set(r, g, b)    do { } while (0)
#endif

#if defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
/* Root of trust for the DTLS connection to coap.golioth.io. Taken from the
 * Pouch module rather than vendored; see loader/CMakeLists.txt. */
static const unsigned char dtls_ca_crt[] = {
#include "pouch-dtls-ca.inc"
};
#endif

#define SEC_TAG ((sec_tag_t) CONFIG_ARDUINO_POUCH_SEC_TAG)

/* Values returned by arduino_pouch_status(). Mirrored in libraries/Golioth. */
#define POUCH_STATUS_IDLE       0
#define POUCH_STATUS_CONNECTING 1
#define POUCH_STATUS_ONLINE     2

static atomic_t glue_status = ATOMIC_INIT(POUCH_STATUS_IDLE);
static int glue_last_err;

/* Entries written since the last successful sync. The Golioth gateway rejects
 * an empty pouch with 4.00, and pouch only queues its header once there is
 * data (src/uplink.c), so syncing with nothing pending is both useless and an
 * error. Downlink therefore only happens alongside uplink or on sync_now(). */
static atomic_t pending_entries = ATOMIC_INIT(0);

static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(got_ip, 0, 1);
static K_SEM_DEFINE(sync_now, 0, 1);

static struct net_mgmt_event_callback ipv4_cb;

/* Credentials in use. Default to the compiled-in pair; a sketch may replace
 * them before begin(). Pouch keeps the certificate pointer for the lifetime of
 * the session, so the buffer must outlive it. */
static const unsigned char *crt_buf = device_crt_der;
static size_t crt_len = sizeof(device_crt_der);
static const unsigned char *key_buf = device_key_der;
static size_t key_len = sizeof(device_key_der);

static void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&got_ip);
	}
}

static bool has_ipv4(struct net_if *iface)
{
	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

/* Import the DER private key into the PSA key store and hand Pouch the id.
 * Mirrors load_private_key() in the Pouch coap_client example. */
static psa_key_id_t import_device_key(void)
{
	mbedtls_pk_context pk;
	psa_key_id_t key_id = PSA_KEY_ID_NULL;

	mbedtls_pk_init(&pk);

	int err = mbedtls_pk_parse_key(&pk, key_buf, key_len, NULL, 0);
	if (err) {
		LOG_ERR("Failed to parse device key: -0x%x", (unsigned int) -err);
		goto out;
	}

	psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_algorithm(&attrs, PSA_ALG_ECDH);
	psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_DERIVE);

	err = mbedtls_pk_import_into_psa(&pk, &attrs, &key_id);
	if (err) {
		LOG_ERR("Failed to import device key: -0x%x", (unsigned int) -err);
		key_id = PSA_KEY_ID_NULL;
	}

out:
	mbedtls_pk_free(&pk);
	return key_id;
}

#if defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
static int load_dtls_credentials(void)
{
	int err;

	err = tls_credential_add(SEC_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, dtls_ca_crt,
				 sizeof(dtls_ca_crt));
	if (err && err != -EEXIST) {
		LOG_ERR("Failed to add CA certificate: %d", err);
		return err;
	}

	err = tls_credential_add(SEC_TAG, TLS_CREDENTIAL_PUBLIC_CERTIFICATE, crt_buf, crt_len);
	if (err && err != -EEXIST) {
		LOG_ERR("Failed to add device certificate: %d", err);
		return err;
	}

	err = tls_credential_add(SEC_TAG, TLS_CREDENTIAL_PRIVATE_KEY, key_buf, key_len);
	if (err && err != -EEXIST) {
		LOG_ERR("Failed to add device key: %d", err);
		return err;
	}

	return 0;
}

static int link_up(void)
{
	struct net_if *iface = NULL;

#if defined(CONFIG_WIFI)
	iface = net_if_get_wifi_sta();
#endif
	if (iface == NULL) {
		iface = net_if_get_default();
	}

	if (iface == NULL) {
		LOG_ERR("No network interface");
		return -ENODEV;
	}

	/* A sketch that called WiFi.begin() has already associated and run DHCP. */
	if (has_ipv4(iface)) {
		LOG_INF("Interface already has an address");
		return 0;
	}

#if defined(CONFIG_WIFI)
	if (sizeof(POUCH_WIFI_SSID) > 1) {
		/*
		 * Field-for-field what libraries/WiFi/src/WiFi.cpp sends. A zeroed
		 * struct is not good enough: WIFI_CHANNEL_ANY is 255 and
		 * WIFI_FREQ_BANDWIDTH_20MHZ is 1, so leaving them at 0 asks the
		 * driver for channel 0 at an unspecified bandwidth.
		 */
		struct wifi_connect_req_params params = {
			.ssid = (const uint8_t *) POUCH_WIFI_SSID,
			.ssid_length = sizeof(POUCH_WIFI_SSID) - 1,
			.psk = (uint8_t *) POUCH_WIFI_PSK,
			.psk_length = sizeof(POUCH_WIFI_PSK) - 1,
			.security = WIFI_SECURITY_TYPE_PSK,
			.channel = WIFI_CHANNEL_ANY,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ,
		};

		/* The NXP STA interface comes up admin-down. */
		if (!net_if_is_up(iface)) {
			LOG_INF("Bringing interface up");
			net_if_up(iface);
		}

		LOG_INF("Associating with \"%s\"", POUCH_WIFI_SSID);

		int err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
		if (err) {
			LOG_ERR("Wi-Fi connect request failed: %d", err);
			return err;
		}

		LOG_INF("Connect request accepted, waiting for association");
		net_mgmt_event_wait_on_iface(iface, NET_EVENT_WIFI_CONNECT_RESULT, NULL, NULL, NULL,
					     K_SECONDS(60));
		LOG_INF("Association phase done");
	} else {
		LOG_INF("No SSID compiled in; connect from the shell:");
		LOG_INF("  wifi connect -s <ssid> -p <psk> -k 1");
	}
#endif /* CONFIG_WIFI */

	/*
	 * Nothing in the loader starts DHCP. The Arduino core only does it from
	 * the WiFi library (libraries/SocketWrapper/SocketHelpers.cpp), which
	 * needs a sketch, and this variant sets no CONFIG_NET_CONFIG_* to do it
	 * automatically. Arm it here; CONFIG_NET_DHCPV4_RESTART_ON_IF_UP=y makes
	 * it safe to call before the link is actually up.
	 */
	if (iface->config.dhcpv4.state == NET_DHCPV4_DISABLED) {
		LOG_INF("Starting DHCPv4");
		net_dhcpv4_start(iface);
	}

	LOG_INF("Waiting for an IPv4 address");
	while (k_sem_take(&got_ip, K_SECONDS(30)) != 0) {
		if (has_ipv4(iface)) {
			break;
		}
		LOG_WRN("Still waiting for an IPv4 address (DHCP state %s)",
			net_dhcpv4_state_name(iface->config.dhcpv4.state));
	}

	char buf[NET_IPV4_ADDR_LEN];
	struct net_in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	LOG_INF("Link up, address %s",
		addr ? net_addr_ntop(AF_INET, addr, buf, sizeof(buf)) : "unknown");

	return 0;
}
#endif /* CONFIG_POUCH_TRANSPORT_COAP_CLIENT */

static int pouch_bring_up(void)
{
	struct pouch_config config = {0};
	int err;

	/* Idempotent; Zephyr's CONFIG_MBEDTLS_INIT normally gets here first. */
	psa_crypto_init();

	config.certificate.buffer = crt_buf;
	config.certificate.size = crt_len;
	config.private_key = import_device_key();

	if (config.private_key == PSA_KEY_ID_NULL) {
		return -EIO;
	}

	err = pouch_init(&config);
	if (err) {
		LOG_ERR("pouch_init failed: %d", err);
		return err;
	}

#if defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
	err = load_dtls_credentials();
	if (err) {
		return err;
	}

	err = pouch_coap_client_init(SEC_TAG);
	if (err) {
		LOG_ERR("pouch_coap_client_init failed: %d", err);
		return err;
	}
#endif

	return 0;
}

#if defined(CONFIG_ARDUINO_POUCH_DEMO_UPLINK)
/*
 * Bring-up traffic. Two entries so the gate exercises multi-entry uplink.
 *
 * The Golioth Logging service has no device-side API in Pouch: the only uplink
 * paths in the tree are Stream (.s/), Settings (.c/status) and OTA (.u/c/).
 * Golioth routes logs through Pipelines, so this writes a CBOR entry to
 * .s/logs to find out whether the default CBOR logs Pipeline picks it up.
 * Unverified - that is what the gate is for.
 */
static void demo_uplink(void)
{
	static unsigned int seq;

	char json[48];
	int n = snprintk(json, sizeof(json), "{\"uptime\":%u,\"seq\":%u}",
			 (unsigned int) (k_uptime_get() / 1000), seq);

	int err = pouch_uplink_entry_write(".s/sensor", POUCH_CONTENT_TYPE_JSON, json, n,
					   POUCH_FOREVER);
	if (err) {
		LOG_WRN("sensor entry failed: %d", err);
	}

	/* CBOR: {"level": "info", "message": "<msg>"} */
	char msg[32];
	int msglen = snprintk(msg, sizeof(msg), "pouch loader alive, seq %u", seq);
	uint8_t cbor[80];
	size_t i = 0;

	cbor[i++] = 0xa2;                            /* map(2) */
	cbor[i++] = 0x65;                            /* text(5) */
	memcpy(&cbor[i], "level", 5); i += 5;
	cbor[i++] = 0x64;                            /* text(4) */
	memcpy(&cbor[i], "info", 4); i += 4;
	cbor[i++] = 0x67;                            /* text(7) */
	memcpy(&cbor[i], "message", 7); i += 7;
	cbor[i++] = 0x78;                            /* text, 1-byte length */
	cbor[i++] = (uint8_t) msglen;
	memcpy(&cbor[i], msg, msglen); i += msglen;

	err = pouch_uplink_entry_write(".s/logs", POUCH_CONTENT_TYPE_CBOR, cbor, i, POUCH_FOREVER);
	if (err) {
		LOG_WRN("logs entry failed: %d", err);
	}

	seq++;
}
POUCH_UPLINK_HANDLER(demo_uplink);
#endif /* CONFIG_ARDUINO_POUCH_DEMO_UPLINK */

/*
 * Loader-to-sketch trampolines.
 *
 * POUCH_EVENT_HANDLER and POUCH_DOWNLINK_HANDLER are linker iterable sections,
 * so an llext cannot register one. The loader owns the single section entry and
 * re-exports a runtime registration function instead - the same shape
 * fixups.c uses to hand INPUT_CALLBACK_DEFINE to sketches.
 *
 * Calling into the llext is safe here: CONFIG_USERSPACE=n and the sketch runs
 * via llext_bootstrap() on a loader thread in supervisor mode, and it is never
 * unloaded, so a registered pointer stays valid. It does borrow the calling
 * thread's stack, which for these is Pouch's own work queue - keep sketch
 * callbacks short.
 *
 * Every pointer is NULL until a sketch registers: CONFIG_ARDUINO_POUCH_AUTOSTART
 * means the session can open before setup() has run.
 */
static arduino_pouch_event_cb_t sketch_event_cb;
static void *sketch_event_ctx;

static arduino_pouch_downlink_start_cb_t sketch_dl_start_cb;
static arduino_pouch_downlink_data_cb_t sketch_dl_data_cb;
static void *sketch_dl_ctx;

/* Set once a sketch asks for downlink; see the sync gate in pouch_thread(). */
static atomic_t downlink_wanted = ATOMIC_INIT(0);

static void glue_event_handler(enum pouch_event event, void *ctx)
{
	ARG_UNUSED(ctx);

	LOG_DBG("Pouch event %d", (int) event);

	if (sketch_event_cb != NULL) {
		sketch_event_cb((int) event, sketch_event_ctx);
	}
}
POUCH_EVENT_HANDLER(glue_event_handler, NULL);

static void glue_downlink_start(unsigned int stream_id, const char *path, uint16_t content_type)
{
	LOG_DBG("Downlink %u open: %s (ct %u)", stream_id, path, content_type);

	if (sketch_dl_start_cb != NULL) {
		sketch_dl_start_cb(stream_id, path, content_type, sketch_dl_ctx);
	}
}

static void glue_downlink_data(unsigned int stream_id, const void *data, size_t len, bool is_last)
{
	if (sketch_dl_data_cb != NULL) {
		sketch_dl_data_cb(stream_id, data, len, is_last ? 1 : 0, sketch_dl_ctx);
	}
}
POUCH_DOWNLINK_HANDLER(glue_downlink_start, glue_downlink_data);

static void pouch_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int err;

	hb_init();
	hb_set(1, 0, 0);   /* red: thread running */

	k_sem_take(&start_sem, K_FOREVER);
	atomic_set(&glue_status, POUCH_STATUS_CONNECTING);

#if defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	err = link_up();
	if (err) {
		glue_last_err = err;
		atomic_set(&glue_status, err);
		return;
	}
#endif

	err = pouch_bring_up();
	if (err) {
		glue_last_err = err;
		atomic_set(&glue_status, err);
		return;
	}

	hb_set(1, 1, 0);   /* yellow: Pouch initialised */

	/*
	 * Online means "transport ready to accept data", not "a round trip just
	 * succeeded". Gating on a completed sync would deadlock: a sketch that
	 * only streams while connected() would never produce the data that makes
	 * the first sync possible.
	 */
	atomic_set(&glue_status, POUCH_STATUS_ONLINE);

#if defined(CONFIG_POUCH_GATEWAY)
	/* Bring up the BLE broker once the cloud side is proven. */
	extern int arduino_pouch_gateway_start(void);

	err = arduino_pouch_gateway_start();
	if (err) {
		LOG_ERR("Gateway start failed: %d", err);
		glue_last_err = err;
	}
#endif

#if defined(CONFIG_POUCH_TRANSPORT_BLE_GATT)
	/*
	 * Device role. There is no sync loop: a gateway collects from us. All we
	 * do is raise the sync-request flag in the advertisement when the sketch
	 * has queued something, and lower it once a gateway has been and gone.
	 */
	extern int arduino_pouch_ble_start(void);
	extern int arduino_pouch_ble_request_sync(int enable);

	err = arduino_pouch_ble_start();
	if (err) {
		glue_last_err = err;
		atomic_set(&glue_status, err);
		return;
	}

	hb_set(0, 1, 0);   /* green: advertising */

	while (true) {
		k_sem_take(&sync_now, K_SECONDS(CONFIG_ARDUINO_POUCH_SYNC_PERIOD_S));

		/*
		 * pending_entries only counts what a sketch wrote through
		 * arduino_pouch_stream(). Entries produced by a POUCH_UPLINK_HANDLER
		 * in the loader - the demo uplink, for one - are written during the
		 * session itself, so they cannot be counted in advance and the device
		 * would never ask for a gateway. Once bonded, pouch's broker ignores a
		 * peripheral that is not requesting sync, so the relay would go quiet
		 * after the first, unbonded connection.
		 */
		if (atomic_get(&pending_entries) > 0
		    || IS_ENABLED(CONFIG_ARDUINO_POUCH_DEMO_UPLINK)) {
			arduino_pouch_ble_request_sync(1);
		}

		/* Blink green so "advertising" is distinguishable from "hung". */
		hb_set(0, 0, 0);
		k_sleep(K_MSEC(120));
		hb_set(0, 1, 0);
	}
#elif defined(CONFIG_POUCH_TRANSPORT_COAP_CLIENT)
	LOG_INF("Pouch ready, syncing every %ds", CONFIG_ARDUINO_POUCH_SYNC_PERIOD_S);

	while (true) {
		bool forced = k_sem_take(&sync_now, K_SECONDS(CONFIG_ARDUINO_POUCH_SYNC_PERIOD_S)) == 0;

		/*
		 * Skipping a sync with nothing queued avoids POSTing an empty pouch,
		 * which the gateway answers with 4.00. But downlink only ever arrives
		 * as the response to a sync, so anything that consumes downlink -
		 * a sketch's downlink handler, Settings, OTA - has to keep the sync
		 * happening regardless of whether we have something to say.
		 */
		bool want_downlink = atomic_get(&downlink_wanted) != 0
				     || IS_ENABLED(CONFIG_GOLIOTH_SETTINGS)
				     || IS_ENABLED(CONFIG_GOLIOTH_OTA);

		if (!IS_ENABLED(CONFIG_ARDUINO_POUCH_DEMO_UPLINK)
		    && !IS_ENABLED(CONFIG_POUCH_GATEWAY) && !forced && !want_downlink
		    && atomic_get(&pending_entries) == 0) {
			continue;
		}

		err = pouch_coap_client_sync();
		if (err) {
			/* Stay online: the link is up, this pouch just did not land. */
			LOG_WRN("Pouch sync failed: %d", err);
			glue_last_err = err;
		} else {
			atomic_clear(&pending_entries);
			glue_last_err = 0;
		}
	}
#else
#error "CONFIG_ARDUINO_POUCH needs a Pouch transport: BLE GATT or CoAP client"
#endif
}

K_THREAD_DEFINE(arduino_pouch_tid, CONFIG_ARDUINO_POUCH_THREAD_STACK_SIZE, pouch_thread, NULL, NULL,
		NULL, K_PRIO_PREEMPT(10), 0, 0);

#if defined(CONFIG_ARDUINO_POUCH_AUTOSTART)
static int arduino_pouch_autostart(void)
{
	k_sem_give(&start_sem);
	return 0;
}
SYS_INIT(arduino_pouch_autostart, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

/*
 * Sketch-facing API. Exported in llext_exports.c; keep the surface small and
 * the signatures free of Pouch and PSA types.
 */

int arduino_pouch_set_credentials(const unsigned char *crt, size_t crtlen,
				  const unsigned char *key, size_t keylen)
{
	if (crt == NULL || crtlen == 0 || key == NULL || keylen == 0) {
		return -EINVAL;
	}

	if (atomic_get(&glue_status) != POUCH_STATUS_IDLE) {
		return -EBUSY;
	}

	crt_buf = crt;
	crt_len = crtlen;
	key_buf = key;
	key_len = keylen;

	return 0;
}

int arduino_pouch_begin(void)
{
	/* Idempotent: the thread takes start_sem exactly once. */
	k_sem_give(&start_sem);
	return 0;
}

int arduino_pouch_stream(const char *path, const void *data, size_t len, uint16_t content_type)
{
	if (path == NULL || data == NULL || len == 0) {
		return -EINVAL;
	}

	if (atomic_get(&glue_status) < POUCH_STATUS_ONLINE) {
		return -EAGAIN;
	}

	int err = pouch_uplink_entry_write(path, content_type, data, len, POUCH_FOREVER);
	if (err == 0) {
		atomic_inc(&pending_entries);
	}

	return err;
}

/* Called by the BLE transport once a gateway has collected and gone. */
void arduino_pouch_mark_flushed(void)
{
	atomic_clear(&pending_entries);
}

int arduino_pouch_status(void)
{
	return (int) atomic_get(&glue_status);
}

int arduino_pouch_sync_now(uint32_t timeout_ms)
{
	if (atomic_get(&glue_status) < POUCH_STATUS_CONNECTING) {
		return -ENOTCONN;
	}

	k_sem_give(&sync_now);

	if (timeout_ms == 0) {
		return 0;
	}

	/*
	 * Wait for the queued entries to actually go out. Status is not the
	 * signal here - it reports link readiness, not delivery - so watch the
	 * pending count, which the sync thread clears only on success.
	 */
	for (uint32_t waited = 0; waited < timeout_ms; waited += 50) {
		if (atomic_get(&pending_entries) == 0) {
			return 0;
		}
		k_sleep(K_MSEC(50));
	}

	return glue_last_err ? glue_last_err : -ETIMEDOUT;
}

int arduino_pouch_on_event(arduino_pouch_event_cb_t cb, void *user_data)
{
	/* Order matters: publish the context before the pointer the trampoline
	 * tests, so a session event landing mid-registration cannot read a stale
	 * context against a fresh callback. */
	sketch_event_ctx = user_data;
	compiler_barrier();
	sketch_event_cb = cb;

	return 0;
}

int arduino_pouch_on_downlink(arduino_pouch_downlink_start_cb_t start_cb,
			      arduino_pouch_downlink_data_cb_t data_cb, void *user_data)
{
	sketch_dl_ctx = user_data;
	compiler_barrier();
	sketch_dl_start_cb = start_cb;
	sketch_dl_data_cb = data_cb;

	/*
	 * Downlink only ever arrives as the response to a sync, and the sync loop
	 * skips syncing when there is nothing queued. Registering a consumer has
	 * to lift that gate or the callbacks would only ever fire on the syncs
	 * that happened to carry uplink.
	 */
	atomic_set(&downlink_wanted, (start_cb != NULL || data_cb != NULL) ? 1 : 0);

	return 0;
}

#endif /* CONFIG_ARDUINO_POUCH */
