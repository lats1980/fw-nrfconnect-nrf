/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/Zephyr/BLEAdvertisingArbiter.h>

#include <bluetooth/services/wifi_provisioning.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

namespace Nrf {

class WPVService {
public:
	bool Init(uint8_t priority, uint16_t minInterval, uint16_t maxInterval);

	bool StartServer();

	void StopServer();

	friend WPVService &GetWPVService();
	static WPVService sInstance;

private:

	static void AuthCancel(struct bt_conn *conn);
	static void PairingComplete(struct bt_conn *conn, bool bonded);
	static void PairingFailed(struct bt_conn *conn, enum bt_security_err reason);

	static struct bt_conn_auth_cb sConnAuthCallbacks;
	static struct bt_conn_auth_info_cb sConnAuthInfoCallbacks;

	bool mIsStarted = false;

	chip::DeviceLayer::BLEAdvertisingArbiter::Request mAdvertisingRequest = {};
	std::array<bt_data, 2> mAdvertisingItems;
	std::array<bt_data, 1> mServiceItems;

};

inline WPVService &GetWPVService()
{
	return WPVService::sInstance;
}

} /* namespace Nrf */
