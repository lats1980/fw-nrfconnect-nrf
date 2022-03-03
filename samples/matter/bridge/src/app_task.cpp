/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "led_widget.h"
#include "zigbee_shell.h"
#include "Device.h"

#include <platform/CHIPDeviceLayer.h>

#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <app-common/zap-generated/af-structs.h>
#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app/reporting/reporting.h>
#include <vector>

#include <dk_buttons_and_leds.h>
#include <logging/log.h>
#include <zephyr.h>

using namespace ::chip;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

LOG_MODULE_DECLARE(app);

namespace
{
static constexpr size_t kAppEventQueueSize = 10;
static constexpr uint32_t kFactoryResetTriggerTimeout = 6000;

K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
LEDWidget sStatusLED;
LEDWidget sUnusedLED;
LEDWidget sUnusedLED_1;
LEDWidget sUnusedLED_2;

ZigbeeShell sZbShell;

#define ZB_HA_DIMMABLE_LIGHT_DEVICE_ID  0x0101
#define ZB_AF_HA_PROFILE_ID		0x0104

static const int kNodeLabelSize = 32;
// Current ZCL implementation of Struct uses a max-size array of 254 bytes
static const int kDescriptorAttributeArraySize = 254;
static const int kFixedLabelAttributeArraySize = 254;

static EndpointId gCurrentEndpointId;
static EndpointId gFirstDynamicEndpointId;
static Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]; // number of dynamic endpoints count

// 4 Bridged devices
//std::vector<Device> Lights(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT, Device("Default", "None"));
std::vector<Device> Lights(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT, Device());

// (taken from chip-devices.xml)
#define DEVICE_TYPE_CHIP_BRIDGE 0x0a0b
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100
// Device Version for dynamic endpoints:
#define DEVICE_VERSION_DEFAULT 1

/* BRIDGED DEVICE ENDPOINT: contains the following clusters:
   - On/Off
   - Descriptor
   - Bridged Device Basic
   - Fixed Label
*/

// Declare On/Off cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_ON_OFF_ATTRIBUTE_ID, BOOLEAN, 1, 0), /* on/off */
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Descriptor cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_DEVICE_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),     /* device list */
	DECLARE_DYNAMIC_ATTRIBUTE(ZCL_SERVER_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* server list */
	DECLARE_DYNAMIC_ATTRIBUTE(ZCL_CLIENT_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* client list */
	DECLARE_DYNAMIC_ATTRIBUTE(ZCL_PARTS_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),  /* parts list */
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Bridged Device Basic information cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_NODE_LABEL_ATTRIBUTE_ID, CHAR_STRING, kNodeLabelSize, 0), /* NodeLabel */
	DECLARE_DYNAMIC_ATTRIBUTE(ZCL_REACHABLE_ATTRIBUTE_ID, BOOLEAN, 1, 0),               /* Reachable */
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Fixed Label cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(fixedLabelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_LABEL_LIST_ATTRIBUTE_ID, ARRAY, kFixedLabelAttributeArraySize, 0), /* label list */
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Cluster List for Bridged Light endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedLightClusters)
DECLARE_DYNAMIC_CLUSTER(ZCL_ON_OFF_CLUSTER_ID, onOffAttrs), DECLARE_DYNAMIC_CLUSTER(ZCL_DESCRIPTOR_CLUSTER_ID, descriptorAttrs),
	DECLARE_DYNAMIC_CLUSTER(ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID, bridgedDeviceBasicAttrs),
	DECLARE_DYNAMIC_CLUSTER(ZCL_FIXED_LABEL_CLUSTER_ID, fixedLabelAttrs) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedLightEndpoint, bridgedLightClusters);

/* REVISION definitions:
 */

#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_REVISION (1u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)

bool sIsThreadProvisioned;
bool sIsThreadEnabled;
bool sHaveBLEConnections;

k_timer sFunctionTimer;
} /* namespace */

AppTask AppTask::sAppTask;

CHIP_ERROR AddDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, uint16_t deviceType)
{
	uint8_t index = 0;
	while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
	{
		if (NULL == gDevices[index])
		{
			gDevices[index] = dev;
			EmberAfStatus ret;
			ret = emberAfSetDynamicEndpoint(index, gCurrentEndpointId, ep, deviceType, DEVICE_VERSION_DEFAULT);
			if (ret == EMBER_ZCL_STATUS_SUCCESS)
			{
				ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),
						gCurrentEndpointId, index);
				gCurrentEndpointId++;
				return CHIP_NO_ERROR;
			}
			else if (ret != EMBER_ZCL_STATUS_DUPLICATE_EXISTS)
			{
				ChipLogProgress(DeviceLayer, "Failed to add dynamic endpoint, Insufficient space");
				return CHIP_ERROR_INTERNAL;
			}
		}
		index++;
	}
	ChipLogProgress(DeviceLayer, "Failed to add dynamic endpoint: No endpoints available!");
	return CHIP_ERROR_INTERNAL;
}

CHIP_ERROR RemoveDeviceEndpoint(Device * dev)
{
	for (uint8_t index = 0; index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT; index++)
	{
		if (gDevices[index] == dev)
		{
			EndpointId ep   = emberAfClearDynamicEndpoint(index);
			gDevices[index] = NULL;
			ChipLogProgress(DeviceLayer, "Removed device %s from dynamic endpoint %d (index=%d)", dev->GetName(), ep, index);
			// Silence complaints about unused ep when progress logging
			// disabled.
			UNUSED_VAR(ep);
			return CHIP_NO_ERROR;
		}
	}
	return CHIP_ERROR_INTERNAL;
}

// ZCL format -> (len, string)
uint8_t * ToZclCharString(uint8_t * zclString, const char * cString, uint8_t maxLength)
{
	size_t len = strlen(cString);
	if (len > maxLength)
	{
		len = maxLength;
	}
	zclString[0] = static_cast<uint8_t>(len);
	memcpy(&zclString[1], cString, zclString[0]);
	return zclString;
}

// Converted into bytes and mapped the (label, value)
void EncodeFixedLabel(const char * label, const char * value, uint8_t * buffer, uint16_t length, EmberAfAttributeMetadata * am)
{
	_LabelStruct labelStruct;

	labelStruct.label = chip::CharSpan::fromCharString(label);
	labelStruct.value = chip::CharSpan::fromCharString(value);

	// TODO: Need to set up an AttributeAccessInterface to handle the lists here.
}

EmberAfStatus HandleReadBridgedDeviceBasicAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
						    uint16_t maxReadLength)
{
	ChipLogProgress(DeviceLayer, "HandleReadBridgedDeviceBasicAttribute: attrId=%d, maxReadLength=%d", attributeId, maxReadLength);

	if ((attributeId == ZCL_REACHABLE_ATTRIBUTE_ID) && (maxReadLength == 1))
	{
		*buffer = dev->IsReachable() ? 1 : 0;
	}
	else if ((attributeId == ZCL_NODE_LABEL_ATTRIBUTE_ID) && (maxReadLength == 32))
	{
		ToZclCharString(buffer, dev->GetName(), static_cast<uint8_t>(maxReadLength - 1));
	}
	else if ((attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
	{
		*buffer = (uint16_t) ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_REVISION;
	}
	else
	{
		return EMBER_ZCL_STATUS_FAILURE;
	}

	return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadFixedLabelAttribute(Device * dev, EmberAfAttributeMetadata * am, uint8_t * buffer, uint16_t maxReadLength)
{
	if ((am->attributeId == ZCL_LABEL_LIST_ATTRIBUTE_ID) && (maxReadLength <= kFixedLabelAttributeArraySize))
	{
		EncodeFixedLabel("room", dev->GetLocation(), buffer, maxReadLength, am);
	}
	else if ((am->attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
	{
		*buffer = (uint16_t) ZCL_FIXED_LABEL_CLUSTER_REVISION;
	}
	else
	{
		return EMBER_ZCL_STATUS_FAILURE;
	}

	return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer, uint16_t maxReadLength)
{
	ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: attrId=%d, maxReadLength=%d", attributeId, maxReadLength);

	if ((attributeId == ZCL_ON_OFF_ATTRIBUTE_ID) && (maxReadLength == 1))
	{
		*buffer = dev->IsOn() ? 1 : 0;
	}
	else if ((attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
	{
		*buffer = (uint16_t) ZCL_ON_OFF_CLUSTER_REVISION;
	}
	else
	{
		return EMBER_ZCL_STATUS_FAILURE;
	}

	return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleWriteOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
	ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: attrId=%d", attributeId);

	ReturnErrorCodeIf((attributeId != ZCL_ON_OFF_ATTRIBUTE_ID) || (!dev->IsReachable()), EMBER_ZCL_STATUS_FAILURE);
	dev->SetOnOff(*buffer == 1);
	sZbShell.ZclCmd(dev->GetZbAddr(), dev->GetZbEp(), ZigbeeShell::kCluster_OnOff, (*buffer == 1)?(true):(false));
	return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
						   EmberAfAttributeMetadata * attributeMetadata, uint8_t * buffer,
						   uint16_t maxReadLength)
{
	uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

	if ((endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) && (gDevices[endpointIndex] != NULL))
	{
		Device * dev = gDevices[endpointIndex];

		if (clusterId == ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID)
		{
			return HandleReadBridgedDeviceBasicAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
		}
		else if (clusterId == ZCL_FIXED_LABEL_CLUSTER_ID)
		{
			return HandleReadFixedLabelAttribute(dev, attributeMetadata, buffer, maxReadLength);
		}
		else if (clusterId == ZCL_ON_OFF_CLUSTER_ID)
		{
			return HandleReadOnOffAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
		}
	}

	return EMBER_ZCL_STATUS_FAILURE;
}

EmberAfStatus emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
						    EmberAfAttributeMetadata * attributeMetadata, uint8_t * buffer)
{
	uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

	if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
	{
		Device * dev = gDevices[endpointIndex];

		if ((dev->IsReachable()) && (clusterId == ZCL_ON_OFF_CLUSTER_ID))
		{
			return HandleWriteOnOffAttribute(dev, attributeMetadata->attributeId, buffer);
		}
	}

	return EMBER_ZCL_STATUS_FAILURE;
}

void HandleDeviceStatusChanged(Device * dev, Device::Changed_t itemChangedMask)
{
	if (itemChangedMask & Device::kChanged_Reachable)
	{
		uint8_t reachable = dev->IsReachable() ? 1 : 0;
		MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID,
						       ZCL_REACHABLE_ATTRIBUTE_ID, CLUSTER_MASK_SERVER, ZCL_BOOLEAN_ATTRIBUTE_TYPE,
						       &reachable);
	}

	if (itemChangedMask & Device::kChanged_State)
	{
		uint8_t isOn = dev->IsOn() ? 1 : 0;
		MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_ON_OFF_CLUSTER_ID, ZCL_ON_OFF_ATTRIBUTE_ID,
						       CLUSTER_MASK_SERVER, ZCL_BOOLEAN_ATTRIBUTE_TYPE, &isOn);
	}

	if (itemChangedMask & Device::kChanged_Name)
	{
		uint8_t zclName[kNodeLabelSize + 1];
		ToZclCharString(zclName, dev->GetName(), kNodeLabelSize);
		MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID,
						       ZCL_NODE_LABEL_ATTRIBUTE_ID, CLUSTER_MASK_SERVER, ZCL_CHAR_STRING_ATTRIBUTE_TYPE,
						       zclName);
	}
	if (itemChangedMask & Device::kChanged_Location)
	{
		uint8_t buffer[kFixedLabelAttributeArraySize];
		EmberAfAttributeMetadata am = { .attributeId  = ZCL_LABEL_LIST_ATTRIBUTE_ID,
						.size         = kFixedLabelAttributeArraySize,
						.defaultValue = static_cast<uint16_t>(0) };

		EncodeFixedLabel("room", dev->GetLocation(), buffer, sizeof(buffer), &am);

		MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_FIXED_LABEL_CLUSTER_ID, ZCL_LABEL_LIST_ATTRIBUTE_ID,
						       CLUSTER_MASK_SERVER, ZCL_ARRAY_ATTRIBUTE_TYPE, buffer);
	}
}

int AppTask::Init()
{
	int ret;

	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

	sStatusLED.Init(DK_LED1);
	sUnusedLED.Init(DK_LED2);
	sUnusedLED_1.Init(DK_LED3);
	sUnusedLED_2.Init(DK_LED4);

	UpdateStatusLED();

	memset(gDevices, 0, sizeof(gDevices));

	// Whenever bridged device changes its state
	for (size_t i = 0; i < Lights.size(); ++i)
	{
		Lights[i].SetChangeCallback(&HandleDeviceStatusChanged);
	}

	/* Init Zigbee stack */
	sZbShell.SetEventCallback(ZigbeeEventHandler);
	ret = sZbShell.BdbStart();
	if (ret) {
		LOG_ERR("BdbStart() failed");
		return ret;
	}

	/* Initialize buttons */
	ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return ret;
	}

	/* Initialize function timer */
	k_timer_init(&sFunctionTimer, &AppTask::TimerEventHandler, nullptr);
	k_timer_user_data_set(&sFunctionTimer, this);

	/* Init ZCL Data Model and start server */
	chip::Server::GetInstance().Init();

	/* Initialize device attestation config */
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

	// Set starting endpoint id where dynamic endpoints will be assigned, which
	// will be the next consecutive endpoint id after the last fixed endpoint.
	gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
		static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
	gCurrentEndpointId = gFirstDynamicEndpointId;

	// Disable last fixed endpoint, which is used as a placeholder for all of the
	// supported clusters so that ZAP will generated the requisite code.
	emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

	return 0;
}

int AppTask::StartApp()
{
	int ret = Init();

	if (ret) {
		LOG_ERR("AppTask.Init() failed");
		return ret;
	}

	AppEvent event = {};

	while (true) {
		k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
		DispatchEvent(event);
	}
}

void AppTask::PostEvent(const AppEvent &event)
{
	if (k_msgq_put(&sAppEventQueue, &event, K_NO_WAIT)) {
		LOG_INF("Failed to post event to app task event queue");
	}
}

void AppTask::DispatchEvent(const AppEvent &event)
{
	int err;
	uint16_t InputCluster[] = {ZigbeeShell::Cluster_t::kCluster_OnOff};
	uint16_t OutputCluster[] = {};

	switch (event.Type) {
	case AppEvent::FunctionPress:
		FunctionPressHandler();
		break;
	case AppEvent::FunctionRelease:
		FunctionReleaseHandler();
		break;
	case AppEvent::FunctionTimer:
		FunctionTimerEventHandler();
		break;
	case AppEvent::UpdateLedState:
		event.UpdateLedStateEvent.LedWidget->UpdateState();
		break;
	case AppEvent::NetworkRejoin:
		err = sZbShell.ZdoMatchDesc(0xfffd, 0xfffd, ZB_AF_HA_PROFILE_ID, 1, InputCluster, 0, OutputCluster);
		if (err) {
			LOG_ERR("Fail to broadcast MatchDesc");
		}
		break;
	case AppEvent::DeviceAnnounceRsp:
		err = sZbShell.ZdoActiveEpReq(event.zdo.addr);
		if (err) {
			LOG_ERR("Fail to request active ep");
		}
		break;
	case AppEvent::ActiveEpRsp:
		err = sZbShell.ZdoSimpleDescReq(event.zdo.addr, event.zdo.ep);
		if (err) {
			LOG_ERR("Fail to request simple descriptor");
		}
		break;
	case AppEvent::SimpleDescRsp:
		err = sZbShell.ZclAttrRead(event.zdo.addr,
					   event.zdo.ep,
					   ZB_AF_HA_PROFILE_ID,
					   ZigbeeShell::kCluster_OnOff,
					   ZigbeeShell::kOnOffAttr_OnOff);
		if (err) {
			LOG_ERR("Fail to request simple descriptor");
		}
		break;
	case AppEvent::StartNetworkSteering:
		err = sZbShell.NetworkSteering();
		if (err) {
			LOG_ERR("Fail to start network steering");
		}
		break;
	default:
		LOG_INF("Unknown event received");
		break;
	}
}

void AppTask::FunctionPressHandler()
{
	sAppTask.StartFunctionTimer(kFactoryResetTriggerTimeout);
	sAppTask.mFunction = TimerFunction::FactoryReset;
}

void AppTask::FunctionReleaseHandler()
{
	if (sAppTask.mFunction == TimerFunction::FactoryReset) {
		sUnusedLED_2.Set(false);
		sUnusedLED_1.Set(false);
		sUnusedLED.Set(false);

		UpdateStatusLED();

		sAppTask.CancelFunctionTimer();
		sAppTask.mFunction = TimerFunction::NoneSelected;
		LOG_INF("Factory Reset has been Canceled");
	}
}

void AppTask::FunctionTimerEventHandler()
{
	if (sAppTask.mFunction == TimerFunction::FactoryReset) {
		sAppTask.mFunction = TimerFunction::NoneSelected;
		LOG_INF("Factory Reset triggered");

		sStatusLED.Set(true);
		sUnusedLED.Set(true);
		sUnusedLED_1.Set(true);
		sUnusedLED_2.Set(true);

		ConfigurationMgr().InitiateFactoryReset();
	}
}

void AppTask::LEDStateUpdateHandler(LEDWidget &ledWidget)
{
	sAppTask.PostEvent(AppEvent{ AppEvent::UpdateLedState, &ledWidget });
}

void AppTask::UpdateStatusLED()
{
	/* Update the status LED.
	 *
	 * If thread and service provisioned, keep the LED On constantly.
	 *
	 * If the system has ble connection(s) uptill the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED On for a very short time. */
	if (sIsThreadProvisioned && sIsThreadEnabled) {
		sStatusLED.Set(true);
	} else if (sHaveBLEConnections) {
		sStatusLED.Blink(100, 100);
	} else {
		sStatusLED.Blink(50, 950);
	}
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		UpdateStatusLED();
		break;
	case DeviceEventType::kThreadStateChange:
		sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
		sIsThreadEnabled = ConnectivityMgr().IsThreadEnabled();
		UpdateStatusLED();
		break;
	default:
		break;
	}
}

void AppTask::ButtonEventHandler(uint32_t buttonState, uint32_t hasChanged)
{
	if (DK_BTN1_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionPress });
	} else if (DK_BTN1_MSK & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionRelease });
	}

	if (DK_BTN2_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::StartNetworkSteering });
	}
}

void AppTask::ZigbeeEventHandler(ZigbeeShell * shell, ZigbeeShell::Event_t event)
{
	LOG_INF("Zigbee event: %d", event);
	switch (event) {
	case ZigbeeShell::kEvent_NetworkRejoin:
		GetAppTask().PostEvent(AppEvent{ AppEvent::NetworkRejoin, shell->mEvent.Bdb});
		break;
	case ZigbeeShell::kEvent_DeviceAnnounceRsp:
		GetAppTask().PostEvent(AppEvent{ AppEvent::DeviceAnnounceRsp, shell->mEvent.Zdo});
		break;
	case ZigbeeShell::kEvent_ActiveEpRsp:
		GetAppTask().PostEvent(AppEvent{ AppEvent::ActiveEpRsp, shell->mEvent.Zdo});
		break;
	case ZigbeeShell::kEvent_SimpleDescRsp:
		LOG_INF("addr:0x%04hx ep:%d dev_id:0x%04hx",
			shell->mEvent.Zdo.addr,
			shell->mEvent.Zdo.ep,
			shell->mEvent.Zdo.dev_id);
		if (shell->mEvent.Zdo.dev_id != ZB_HA_DIMMABLE_LIGHT_DEVICE_ID) {
			return;
		}
		for (auto &light : Lights)
		{
			if ((light.GetZbAddr() == shell->mEvent.Zcl.addr) &&
				(light.GetZbEp() == shell->mEvent.Zcl.ep)) {
				LOG_INF("Device existed");
				return;
			}
		}
		for (auto &light : Lights)
		{
			if (!strcmp(light.GetName(), "none")) {
				AddDeviceEndpoint(&light, &bridgedLightEndpoint, DEVICE_TYPE_LO_ON_OFF_LIGHT);
				light.SetName("Light");
				light.SetZbAddr(shell->mEvent.Zdo.addr);
				light.SetZbEp(shell->mEvent.Zdo.ep);
				light.SetReachable(true);
				GetAppTask().PostEvent(AppEvent{ AppEvent::SimpleDescRsp, shell->mEvent.Zdo});
				break;
			}
		}
		break;
	case ZigbeeShell::kEvent_ZclAttrRead:
		for (auto &light : Lights)
		{
			if ((light.GetZbAddr() != shell->mEvent.Zcl.addr) ||
				(light.GetZbEp() != shell->mEvent.Zcl.ep)) {
				continue;
			}
			if (shell->mEvent.Zcl.cluster_id == ZigbeeShell::kCluster_OnOff &&
				shell->mEvent.Zcl.attr_id == ZigbeeShell::kOnOffAttr_OnOff &&
				shell->mEvent.Zcl.type == ZigbeeShell::kZclAttrType_BOOL) {
				if (!strncmp(shell->mEvent.Zcl.value, "True", shell->mEvent.Zcl.len)) {
					light.SetOnOff(true);
				} else if (!strncmp(shell->mEvent.Zcl.value, "False", shell->mEvent.Zcl.len)) {
					light.SetOnOff(false);
				} else {
					LOG_ERR("Wrong attr value");
				}
				break;
			}
		}
		break;
	default:
		LOG_WRN("Unknown event received");
		break;
	}

}

void AppTask::CancelFunctionTimer()
{
	k_timer_stop(&sFunctionTimer);
}

void AppTask::StartFunctionTimer(uint32_t timeoutInMs)
{
	k_timer_start(&sFunctionTimer, K_MSEC(timeoutInMs), K_NO_WAIT);
}

void AppTask::TimerEventHandler(k_timer *timer)
{
	GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionTimer });
}
