/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "light_switch.h"
#include "app_event.h"
#include "binding_handler.h"

#include <app/server/Server.h>
#include <app/util/binding-table.h>
#include <controller/InvokeInteraction.h>

#include <app/clusters/switch-server/switch-server.h>
#include <app-common/zap-generated/attributes/Accessors.h>

using namespace chip;
using namespace chip::app;

void LightSwitch::Init(chip::EndpointId aLightSwitchEndpoint, chip::EndpointId aGenericSwitchEndpoint)
{
	BindingHandler::GetInstance().Init();
	mLightSwitchEndpoint = aLightSwitchEndpoint;
	mGenericSwitchEndpoint = aGenericSwitchEndpoint;
}

void LightSwitch::InitiateActionSwitch(Action mAction)
{
	BindingHandler::BindingData *data = Platform::New<BindingHandler::BindingData>();
	if (data) {
		data->EndpointId = mLightSwitchEndpoint;
		data->ClusterId = Clusters::OnOff::Id;
		switch (mAction) {
		case Action::Toggle:
			data->CommandId = Clusters::OnOff::Commands::Toggle::Id;
			break;
		case Action::On:
			data->CommandId = Clusters::OnOff::Commands::On::Id;
			break;
		case Action::Off:
			data->CommandId = Clusters::OnOff::Commands::Off::Id;
			break;
		default:
			Platform::Delete(data);
			return;
		}
		data->IsGroup = BindingHandler::GetInstance().IsGroupBound();
		DeviceLayer::PlatformMgr().ScheduleWork(BindingHandler::SwitchWorkerHandler,
							reinterpret_cast<intptr_t>(data));
	}
}

void LightSwitch::DimmerChangeBrightness()
{
	static uint16_t sBrightness;
	BindingHandler::BindingData *data = Platform::New<BindingHandler::BindingData>();
	if (data) {
		data->EndpointId = mLightSwitchEndpoint;
		data->CommandId = Clusters::LevelControl::Commands::MoveToLevel::Id;
		data->ClusterId = Clusters::LevelControl::Id;
		/* add to brightness 3 to approximate 1% step of brightness after each call dimmer change. */
		sBrightness += kOnePercentBrightnessApproximation;
		if (sBrightness > kMaximumBrightness) {
			sBrightness = 0;
		}
		data->Value = (uint8_t)sBrightness;
		data->IsGroup = BindingHandler::GetInstance().IsGroupBound();
		DeviceLayer::PlatformMgr().ScheduleWork(BindingHandler::SwitchWorkerHandler,
							reinterpret_cast<intptr_t>(data));
	}
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