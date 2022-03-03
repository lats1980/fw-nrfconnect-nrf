/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

#include "led_widget.h"
#include "zigbee_shell.h"

struct AppEvent {
	enum LightEventType : uint8_t { On, Off, Toggle, Level };

	enum EventType : uint8_t { FunctionPress = Level + 1, FunctionRelease, FunctionTimer };

	enum UpdateLedStateEventType : uint8_t { UpdateLedState = FunctionTimer + 1 };

	enum ZigbeeShellEventType : uint8_t {
		NetworkRejoin = UpdateLedState + 1,
		DeviceAnnounceRsp,
		ActiveEpRsp,
		SimpleDescRsp,
		StartNetworkSteering
	};

	AppEvent() = default;
	explicit AppEvent(EventType type) : Type(type) {}
	AppEvent(UpdateLedStateEventType type, LEDWidget *ledWidget) : Type(type), UpdateLedStateEvent{ ledWidget } {}
	AppEvent(ZigbeeShellEventType type) : Type(type) {}
	AppEvent(ZigbeeShellEventType type, struct ZigbeeShell::BdbEvent bdbEvent) : Type(type), bdb(bdbEvent) {}
	AppEvent(ZigbeeShellEventType type, struct ZigbeeShell::ZdoEvent zdoEvent) : Type(type), zdo{ zdoEvent } {}

	uint8_t Type;
	union {
		struct {
			LEDWidget *LedWidget;
		} UpdateLedStateEvent;
		struct ZigbeeShell::BdbEvent bdb;
		struct ZigbeeShell::ZdoEvent zdo;
	};
};
