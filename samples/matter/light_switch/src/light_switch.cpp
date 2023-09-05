/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "light_switch.h"
#include "app_event.h"
#include "binding_handler.h"
#ifdef CONFIG_CHIP_NUS
#include "bt_nus_service.h"
#endif

#include <app/server/Server.h>
#include <app/util/binding-table.h>
#include <controller/InvokeInteraction.h>

#include <stdio.h>
#include <stdlib.h>

using namespace chip;
using namespace chip::app;

void LightSwitch::Init(chip::EndpointId aLightSwitchEndpoint, uint32_t aGpioPin)
{
	mLightSwitchEndpoint = aLightSwitchEndpoint;
	mGpioPin = aGpioPin;
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

void LightSwitch::OnDeviceConnectedFn(void * context, Messaging::ExchangeManager & exchangeMgr,
                                         const SessionHandle & sessionHandle)
{
	CHIP_ERROR ret = CHIP_NO_ERROR;
	BindingTable &bindingTable = BindingTable::GetInstance();

	auto onSuccessCb = [](const app::ConcreteDataAttributePath & attributePath, const auto & dataResponse) {
		ClusterId clusterId = attributePath.mClusterId;
		AttributeId attributeId = attributePath.mAttributeId;
		EndpointId endpointId = attributePath.mEndpointId;
#ifdef CONFIG_CHIP_NUS
		static char buffer[20];
#endif
		if (dataResponse == true) {
			ChipLogError(NotSpecified, "Update attribte: on");
#ifdef CONFIG_CHIP_NUS
			sprintf(buffer, "switch on");
			GetNUSService().SendData(buffer, sizeof(buffer));
#endif
		} else {
			ChipLogError(NotSpecified, "Update attribte: off");
#ifdef CONFIG_CHIP_NUS
			sprintf(buffer, "switch off");
			GetNUSService().SendData(buffer, sizeof(buffer));
#endif
		}
	};

	auto onFailureCb = [](const app::ConcreteDataAttributePath * attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Update attribute failed: %" CHIP_ERROR_FORMAT, error.Format());
	};
	auto onSubscriptionEstablishedCb = [](const app::ReadClient & readClient, chip::SubscriptionId aSubscriptionId) {
        	ChipLogError(NotSpecified, "SubscribeAttribute command onSubscriptionEstablishedCb");
	};
	ChipLogError(NotSpecified, "Connect to node: %d", (int)sessionHandle->GetPeer().GetNodeId());
	for (auto &entry : bindingTable) {
		switch (entry.type) {
		case EMBER_UNICAST_BINDING:
			if (entry.nodeId == sessionHandle->GetPeer().GetNodeId()) {
				ChipLogError(NotSpecified, "Subscribe onoff attribute of EP: %u", entry.remote, (int)sessionHandle->GetPeer().GetNodeId());
				ret = Controller::SubscribeAttribute<Clusters::OnOff::Attributes::OnOff::TypeInfo>(& exchangeMgr,
										sessionHandle, entry.remote, onSuccessCb, onFailureCb,
										0, 20, onSubscriptionEstablishedCb, nullptr, false, true);
				if (CHIP_NO_ERROR != ret) {
					ChipLogError(NotSpecified, "Subscribe Command Request ERROR: %s", ErrorStr(ret));
				}
			}
			break;
		case EMBER_MULTICAST_BINDING:
			break;
		case EMBER_UNUSED_BINDING:
			break;
		default:
			break;
		}
	}
}

void LightSwitch::OnDeviceConnectionFailureFn(void * context, const ScopedNodeId & peerId, CHIP_ERROR err)
{
	/* TODO: consider retry subscription later */
	ChipLogError(NotSpecified, "Fail to subscribe to bounded device.");
}

void LightSwitch::SubscribeAttribute(void)
{
	BindingTable &bindingTable = BindingTable::GetInstance();
	chip::Server *server = &(chip::Server::GetInstance());

	for (auto &entry : bindingTable) {
		switch (entry.type) {
		case EMBER_UNICAST_BINDING:
			ChipLogError(NotSpecified, "SubscribeAttribute: Connect to %d", (int)entry.nodeId);
			server->GetCASESessionManager()->FindOrEstablishSession(ScopedNodeId(entry.nodeId, entry.fabricIndex),
                                                            &mOnDeviceConnectedCallback, &mOnDeviceConnectionFailureCallback);
			k_sleep(K_MSEC(1000));
			break;
		case EMBER_MULTICAST_BINDING:
			break;
		case EMBER_UNUSED_BINDING:
			break;
		default:
			break;
		}
	}
}