/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bt_wifi_prov_service.h"
#include <net/wifi_mgmt_ext.h>
#include <bluetooth/services/wifi_provisioning.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <platform/CHIPDeviceLayer.h>

using namespace ::chip;
using namespace ::chip::DeviceLayer;

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

#define PROV_BT_NAME (CONFIG_BT_DEVICE_NAME "_PROV")

namespace
{
constexpr uint32_t kAdvertisingOptions = BT_LE_ADV_OPT_CONNECTABLE;
constexpr uint8_t kAdvertisingFlags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
constexpr uint8_t kBTUuid[] = { BT_UUID_PROV_VAL, 0x00, 0x00, 0x00, 0x00 };
} /* namespace */

bt_conn_auth_cb Nrf::WPVService::sConnAuthCallbacks = {
	.cancel = AuthCancel,
};

bt_conn_auth_info_cb Nrf::WPVService::sConnAuthInfoCallbacks = {
	.pairing_complete = PairingComplete,
	.pairing_failed = PairingFailed,
};

namespace Nrf {

WPVService WPVService::sInstance;

bool WPVService::Init(uint8_t priority, uint16_t minInterval, uint16_t maxInterval)
{
	mAdvertisingItems[0] = BT_DATA(BT_DATA_FLAGS, &kAdvertisingFlags, sizeof(kAdvertisingFlags));
	mAdvertisingItems[1] = BT_DATA(BT_DATA_NAME_COMPLETE, PROV_BT_NAME, strlen(PROV_BT_NAME));

	mServiceItems[0] = BT_DATA(BT_DATA_SVC_DATA128, kBTUuid, sizeof(kBTUuid));

	mAdvertisingRequest.priority = priority;
	mAdvertisingRequest.options = kAdvertisingOptions;
	mAdvertisingRequest.minInterval = minInterval;
	mAdvertisingRequest.maxInterval = maxInterval;
	mAdvertisingRequest.advertisingData = Span<bt_data>(mAdvertisingItems);
	mAdvertisingRequest.scanResponseData = Span<bt_data>(mServiceItems);

	mAdvertisingRequest.onStarted = [](int rc) {
		if (rc == 0) {
			GetWPVService().mIsStarted = true;
			LOG_DBG("WPV BLE advertising started");
		} else {
			LOG_ERR("Failed to start WPV BLE advertising: %d", rc);
		}
	};
	mAdvertisingRequest.onStopped = []() {
		GetWPVService().mIsStarted = false;
		LOG_DBG("WPV BLE advertising stopped");
	};

	return true;
}

bool WPVService::StartServer()
{
	struct net_if *iface = net_if_get_default();
	if (mIsStarted) {
		LOG_WRN("WPV service was already started");
		return false;
	}

	if (bt_conn_auth_cb_register(&sConnAuthCallbacks) != 0)
		return false;
	if (bt_conn_auth_info_cb_register(&sConnAuthInfoCallbacks) != 0)
		return false;
	if (bt_wifi_prov_init() != 0)
		return false;

	PlatformMgr().LockChipStack();
	CHIP_ERROR ret = BLEAdvertisingArbiter::InsertRequest(mAdvertisingRequest);
	PlatformMgr().UnlockChipStack();

	if (CHIP_NO_ERROR != ret) {
		LOG_ERR("Could not start Wi-Fi provisioning service");
		return false;
	}

	/* Apply stored wifi credential */
	net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

	return true;
}

void WPVService::StopServer()
{
	if (!mIsStarted)
		return;

	PlatformMgr().LockChipStack();
	BLEAdvertisingArbiter::CancelRequest(mAdvertisingRequest);
	PlatformMgr().UnlockChipStack();
}

void WPVService::AuthCancel(bt_conn *conn)
{
	LOG_INF("WPV Pairing cancelled");

	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void WPVService::PairingComplete(bt_conn *conn, bool bonded)
{
	if (!GetWPVService().mIsStarted)
		return;
}

void WPVService::PairingFailed(bt_conn *conn, enum bt_security_err reason)
{
	if (!GetWPVService().mIsStarted)
		return;
}

} /* namespace Nrf */
