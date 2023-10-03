/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>
#include <zephyr/kernel.h>
#include <app/server/Server.h>

using namespace chip;

class RelayWidget {
public:
	static void InitGpio();
	void Init(chip::EndpointId aLightSwitchEndpoint, uint32_t gpioNum);
	void Set(bool state);
	void Invert();
	chip::EndpointId GetRelayEndpointId() { return mRelayEndpoint; }

private:
	uint32_t mGPIONum;
	bool mState;
	chip::EndpointId mRelayEndpoint;
	void DoSet(bool state);
};
