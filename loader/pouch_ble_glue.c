/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Golioth Pouch device over BLE GATT.
 *
 * The device never talks to Golioth directly. It advertises the Pouch service
 * (0xFC49) with a sync-request flag; a gateway scans, connects, and relays the
 * pouch. Authentication is end to end on this device's own certificate, so the
 * gateway is only a courier and does not need to trust or identify us.
 *
 * Requires Zephyr's Bluetooth host (BT_PERIPHERAL), which is mutually exclusive
 * with the CONFIG_BT_HCI_RAW arrangement ArduinoBLE uses. See the Bluetooth
 * block in the board's variant .conf.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ARDUINO_POUCH) && defined(CONFIG_POUCH_TRANSPORT_BLE_GATT)

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(arduino_pouch_ble, CONFIG_ARDUINO_POUCH_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <pouch/transport/bluetooth/gatt.h>

static struct pouch_gatt_adv service_data = POUCH_GATT_ADV_DATA_INIT;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_SVC_DATA16, &service_data, sizeof(service_data)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int advertising_start(void)
{
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
}

/*
 * Ask a gateway to come and collect. Once bonded, a gateway ignores us unless
 * this flag is set (see sync_requested() in pouch's broker scan), which is what
 * makes the radio quiet when there is nothing to send.
 */
int arduino_pouch_ble_request_sync(int enable)
{
	pouch_gatt_adv_req_sync(&service_data, enable != 0);

	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (err == -EAGAIN) {
		/* Not advertising right now - a gateway is connected, which is
		 * exactly when we do not need to ask for one. */
		return 0;
	}
	if (err) {
		LOG_WRN("Failed to update advertising data: %d", err);
	}

	return err;
}

static void resume_advertising(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = advertising_start();
	if (err) {
		LOG_ERR("Failed to resume advertising: %d", err);
	}
}
static K_WORK_DEFINE(resume_work, resume_advertising);

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("Gateway connection failed: 0x%02x", err);
		return;
	}

	LOG_INF("Gateway connected");
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("Gateway disconnected (reason 0x%02x)", reason);

	/* The pouch went out with the gateway, so stop asking until the sketch
	 * queues something new. */
	pouch_gatt_adv_req_sync(&service_data, false);
	k_work_submit(&resume_work);
}

BT_CONN_CB_DEFINE(arduino_pouch_ble_conn_cbs) = {
	.connected = ble_connected,
	.disconnected = ble_disconnected,
};

int arduino_pouch_ble_start(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	err = advertising_start();
	if (err) {
		LOG_ERR("Failed to start advertising: %d", err);
		return err;
	}

	LOG_INF("Advertising as \"%s\", waiting for a gateway", CONFIG_BT_DEVICE_NAME);

	return 0;
}

#endif /* CONFIG_ARDUINO_POUCH && CONFIG_POUCH_TRANSPORT_BLE_GATT */
