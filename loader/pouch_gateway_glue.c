/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Golioth Pouch gateway: BLE GATT broker forwarding to the CoAP cloud transport.
 *
 * The gateway does not authorize devices. It forwards each peripheral's device
 * certificate to Golioth and relays opaque pouches; the peripheral authenticates
 * end to end on its own certificate and Golioth decides whether it belongs to
 * the project. Bonding is a link-layer window for accepting new pairings, not an
 * identity check.
 *
 * Security level follows pouch's own default: with
 * CONFIG_POUCH_GATEWAY_GATT_SCAN_FILTER_BONDED=n the broker asks for
 * BT_SECURITY_L2 and pairing is Just Works, so no passkey UI is needed. Turning
 * that option on moves to L4 and requires numeric comparison, at which point a
 * sketch has to drive the confirmation.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ARDUINO_POUCH) && defined(CONFIG_POUCH_GATEWAY)

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(arduino_pouch_gw, CONFIG_ARDUINO_POUCH_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <pouch/gateway/bt/bond.h>
#include <pouch/gateway/bt/connect.h>
#include <pouch/gateway/bt/scan.h>
#include <pouch/gateway/cert.h>
#include <pouch/gateway/cloud.h>
#include <pouch/gateway/uplink.h>
#include <pouch/transport/coap/gateway.h>

/* L4 (authenticated) only when scanning is restricted to bonded peers. */
static const bt_security_t gw_security =
	IS_ENABLED(CONFIG_POUCH_GATEWAY_GATT_SCAN_FILTER_BONDED) ? BT_SECURITY_L4 : BT_SECURITY_L2;

static void on_gateway_end(struct bt_conn *conn)
{
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void gw_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("Failed to connect to %s (%u)", addr, err);
		bt_conn_unref(conn);
		pouch_gateway_scan_start();
		return;
	}

	LOG_INF("Peripheral connected: %s", addr);

	err = bt_conn_set_security(conn, gw_security);
	if (err) {
		LOG_ERR("Failed to set security: %d", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void gw_disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Peripheral disconnected: %s (reason 0x%02x)", addr, reason);

	bt_conn_unref(conn);
	pouch_gateway_scan_start();
}

static void gw_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		struct bt_conn_info info;

		LOG_ERR("Security change failed at level %d: %u", level, err);

		/* Drop the bond so a retry can pair cleanly rather than
		 * failing forever against a half-established one. */
		if (bt_conn_get_info(conn, &info) == 0) {
			bt_unpair(info.id, info.le.dst);
		}
		bt_conn_disconnect(conn, BT_HCI_ERR_INSUFFICIENT_SECURITY);
		return;
	}

	LOG_INF("Security level %u, starting pouch relay", level);
	pouch_gateway_bt_start(conn, on_gateway_end);
}

BT_CONN_CB_DEFINE(arduino_pouch_gw_conn_cbs) = {
	.connected = gw_connected,
	.disconnected = gw_disconnected,
	.security_changed = gw_security_changed,
};

static void gw_auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing cancelled: %s", addr);
}

/* No passkey callbacks: at BT_SECURITY_L2 pairing is Just Works. Registering
 * passkey_confirm here would advertise a display/yes-no capability we do not
 * have and push the peer into numeric comparison. */
static struct bt_conn_auth_cb gw_auth_cb = {
	.cancel = gw_auth_cancel,
};

static void gw_pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing complete (bonded=%d)", (int) bonded);
}

static void gw_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("Pairing failed: %u", reason);
}

static struct bt_conn_auth_info_cb gw_auth_info_cb = {
	.pairing_complete = gw_pairing_complete,
	.pairing_failed = gw_pairing_failed,
};

/*
 * Called from pouch_glue.c once pouch_init() and pouch_coap_client_init() have
 * succeeded and the link is up.
 */
int arduino_pouch_gateway_start(void)
{
	int err;

	pouch_coap_gateway_init();

	/* Fetch the server certificate before any peripheral connects, so the
	 * first device to arrive is not made to wait for it. */
	for (int attempt = 0; attempt < 12; attempt++) {
		err = pouch_gateway_cloud_ensure_ready();
		if (!err) {
			break;
		}
		LOG_WRN("Cloud transport not ready yet: %d, retrying", err);
		k_sleep(K_SECONDS(5));
	}
	if (err) {
		LOG_ERR("Gateway cloud transport never became ready: %d", err);
		return err;
	}

	pouch_gateway_cert_module_init();
	pouch_gateway_uplink_module_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_BT_SMP)) {
		bt_conn_auth_cb_register(&gw_auth_cb);
		bt_conn_auth_info_cb_register(&gw_auth_info_cb);
	}

	/*
	 * Leave the bonding window open. Authorization is Golioth's decision on
	 * the device certificate, not ours, so a permanently open window costs
	 * nothing in trust terms - but it does let any nearby pouch peripheral
	 * pair with this gateway, so a product would narrow it.
	 * arduino_pouch_gateway_bonding() lets a sketch do exactly that.
	 */
	pouch_gateway_bonding_enable(K_FOREVER);

	pouch_gateway_scan_start();
	LOG_INF("Gateway scanning for pouch peripherals");

	return 0;
}

/* Sketch-facing: narrow or reopen the bonding window. seconds == 0 closes it,
 * a negative value leaves it open indefinitely. */
int arduino_pouch_gateway_bonding(int seconds)
{
	if (seconds == 0) {
		pouch_gateway_bonding_disable();
	} else if (seconds < 0) {
		pouch_gateway_bonding_enable(K_FOREVER);
	} else {
		pouch_gateway_bonding_enable(K_SECONDS(seconds));
	}

	return pouch_gateway_bonding_is_enabled() ? 1 : 0;
}

#endif /* CONFIG_ARDUINO_POUCH && CONFIG_POUCH_GATEWAY */
