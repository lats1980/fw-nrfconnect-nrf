/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once
#include <app/util/basic-types.h>
#include <lib/core/CHIPError.h>

#include <atomic>

/** @class LightSwitch
 *  @brief Class for controlling a CHIP light bulb over a Thread network
 *
 *  Features:
 *  - discovering a CHIP light bulb which advertises itself by sending Thread multicast packets
 *  - toggling and dimming the connected CHIP light bulb by sending appropriate CHIP messages
 */
class LightSwitch {
public:
	void Init(chip::EndpointId aGenericSwitchEndpoint);
	void GenericSwitchShortPress();
	void GenericSwitchLongPress();

	static LightSwitch &GetInstance()
	{
		static LightSwitch sLightSwitch;
		return sLightSwitch;
	}

private:
	chip::EndpointId mGenericSwitchEndpoint;
};
