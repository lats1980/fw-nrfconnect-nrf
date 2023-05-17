/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once
#include <app/util/basic-types.h>
#include <lib/core/CHIPError.h>

#include <atomic>

/** @class GenericSwitch
 *  @brief Class for controlling a CHIP generic switch over a Thread network
 */
class GenericSwitch {
public:
	void Init(chip::EndpointId aGenericSwitchEndpoint);
	void GenericSwitchShortPress();
	void GenericSwitchLongPress();

	static GenericSwitch &GetInstance()
	{
		static GenericSwitch sGenericSwitch;
		return sGenericSwitch;
	}

private:
	chip::EndpointId mGenericSwitchEndpoint;
};