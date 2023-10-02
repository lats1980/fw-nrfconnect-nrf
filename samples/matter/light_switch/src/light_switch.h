/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once
#include <app/util/basic-types.h>
#include <lib/core/CHIPError.h>
#include <app/ReadHandler.h>
#include <controller/ReadInteraction.h>
#include <atomic>

#include "led_widget.h"

using namespace chip;

/** @class LightSwitch
 *  @brief Class for controlling a CHIP light bulb over a Thread network
 *
 *  Features:
 *  - discovering a CHIP light bulb which advertises itself by sending Thread multicast packets
 *  - toggling and dimming the connected CHIP light bulb by sending appropriate CHIP messages
 */
class LightSwitch {
public:
	enum class Action : uint8_t {
		Toggle, /* Switch state on lighting-app device */
		On, /* Turn on light on lighting-app device */
		Off /* Turn off light on lighting-app device */
	};
	LightSwitch() :
		mOnDeviceConnectedCallback(OnDeviceConnectedFn, this),
		mOnDeviceConnectionFailureCallback(OnDeviceConnectionFailureFn, this) {};

	void Init(chip::EndpointId aLightSwitchEndpoint, uint32_t aGpioPin);
	void InitiateActionSwitch(Action);
	chip::EndpointId GetLightSwitchEndpointId() { return mLightSwitchEndpoint; }
	uint32_t GetGpioPin() { return mGpioPin; }
	void SetLED(LEDWidget *aLED) { mLED = aLED; }
	LEDWidget* GetLED() { return mLED; }
	void SubscribeAttribute();

private:
	constexpr static auto kOnePercentBrightnessApproximation = 3;
	constexpr static auto kMaximumBrightness = 254;

	static void OnDeviceConnectedFn(void * context, chip::Messaging::ExchangeManager & exchangeMgr,
					const chip::SessionHandle & sessionHandle);
	static void OnDeviceConnectionFailureFn(void * context, const ScopedNodeId & peerId, CHIP_ERROR error);
	chip::Callback::Callback<chip::OnDeviceConnected> mOnDeviceConnectedCallback;
	chip::Callback::Callback<chip::OnDeviceConnectionFailure> mOnDeviceConnectionFailureCallback;

	chip::EndpointId mLightSwitchEndpoint;
	uint32_t mGpioPin;
	LEDWidget *mLED;
};
