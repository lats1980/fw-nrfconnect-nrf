/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "light_switch.h"
#include "app_event.h"

#include <app/clusters/switch-server/switch-server.h>
#include <app-common/zap-generated/attributes/Accessors.h>

#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>

using namespace chip;
using namespace chip::app;

void LightSwitch::Init(chip::EndpointId aGenericSwitchEndpoint)
{
	mGenericSwitchEndpoint = aGenericSwitchEndpoint;
}

void LightSwitch::GenericSwitchShortPress()
{
	DeviceLayer::SystemLayer().ScheduleLambda([this] {
		Clusters::Switch::Attributes::CurrentPosition::Set(mGenericSwitchEndpoint, 1);
		Clusters::SwitchServer::Instance().OnInitialPress(mGenericSwitchEndpoint, 1);
		Clusters::Switch::Attributes::CurrentPosition::Set(mGenericSwitchEndpoint, 0);
		Clusters::SwitchServer::Instance().OnShortRelease(mGenericSwitchEndpoint, 0);
	});
}


void LightSwitch::GenericSwitchLongPress()
{
	DeviceLayer::SystemLayer().ScheduleLambda([this] {
		Clusters::Switch::Attributes::CurrentPosition::Set(mGenericSwitchEndpoint, 1);
		Clusters::SwitchServer::Instance().OnLongPress(mGenericSwitchEndpoint, 1);
		Clusters::Switch::Attributes::CurrentPosition::Set(mGenericSwitchEndpoint, 0);
		Clusters::SwitchServer::Instance().OnLongRelease(mGenericSwitchEndpoint, 0);
	});
}
