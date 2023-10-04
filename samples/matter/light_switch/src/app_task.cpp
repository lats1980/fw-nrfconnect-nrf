/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app_config.h"
#include "led_util.h"
#include "light_switch.h"
//#include "pwm_device.h"
#include "binding_handler.h"

#ifdef CONFIG_CHIP_NUS
#include "bt_nus_service.h"
#endif

#include <platform/CHIPDeviceLayer.h>

#include "board_util.h"
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <app/DeferredAttributePersistenceProvider.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SafeInt.h>

#include <system/SystemError.h>

#ifdef CONFIG_CHIP_WIFI
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <platform/nrfconnect/wifi/NrfWiFiDriver.h>
#endif

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#ifdef CONFIG_CHIP_ICD_SUBSCRIPTION_HANDLING
#include <app/InteractionModelEngine.h>
#endif

#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr uint32_t kFactoryResetTriggerTimeout = 3000;
constexpr uint32_t kFactoryResetCancelWindowTimeout = 3000;
constexpr size_t kAppEventQueueSize = 10;
constexpr EndpointId kOnOffRelayEndpointId_1 = 1;
constexpr EndpointId kOnOffRelayEndpointId_2 = 2;
constexpr EndpointId kOnOffRelayEndpointId_3 = 3;
constexpr EndpointId kOnOffRelayEndpointId_4 = 4;
constexpr EndpointId kLightEndpointId = 1;

#ifdef CONFIG_CHIP_NUS
constexpr uint16_t kAdvertisingIntervalMin = 400;
constexpr uint16_t kAdvertisingIntervalMax = 500;
constexpr uint8_t kSwitchNUSPriority = 2;
#endif
K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
k_timer sFunctionTimer;

Identify sIdentify = { kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
		       EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_VISIBLE_LED };

LEDWidget *sStatusLED;
LEDWidget *sIdentifyLED;
LEDWidget sOnOffLED_1;
LEDWidget sOnOffLED_2;
LEDWidget sOnOffLED_3;
LEDWidget sOnOffLED_4;
#if NUMBER_OF_LEDS == 4
FactoryResetLEDsWrapper<2> sFactoryResetLEDs{ { FACTORY_RESET_SIGNAL_LED, FACTORY_RESET_SIGNAL_LED1 } };
#endif

bool sIsNetworkProvisioned = false;
bool sIsNetworkEnabled = false;
bool sHaveBLEConnections = false;

} /* namespace */

namespace LedConsts
{
constexpr uint32_t kBlinkRate_ms{ 500 };
constexpr uint32_t kIdentifyBlinkRate_ms{ 500 };

namespace StatusLed
{
	namespace Unprovisioned
	{
		constexpr uint32_t kOn_ms{ 100 };
		constexpr uint32_t kOff_ms{ kOn_ms };
	} /* namespace Unprovisioned */
	namespace Provisioned
	{
		constexpr uint32_t kOn_ms{ 50 };
		constexpr uint32_t kOff_ms{ 950 };
	} /* namespace Provisioned */

} /* namespace StatusLed */
} /* namespace LedConsts */

#ifdef CONFIG_CHIP_WIFI
app::Clusters::NetworkCommissioning::Instance
	sWiFiCommissioningInstance(0, &(NetworkCommissioning::NrfWiFiDriver::Instance()));
#endif

CHIP_ERROR AppTask::Init()
{
	/* Initialize CHIP stack */
	LOG_INF("Init CHIP stack");

	CHIP_ERROR err = chip::Platform::MemoryInit();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Platform::MemoryInit() failed");
		return err;
	}

	err = PlatformMgr().InitChipStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().InitChipStack() failed");
		return err;
	}

#if defined(CONFIG_NET_L2_OPENTHREAD)
	err = ThreadStackMgr().InitThreadStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed: %s", ErrorStr(err));
		return err;
	}

	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_Router);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed: %s", ErrorStr(err));
		return err;
	}

#elif defined(CONFIG_CHIP_WIFI)
	sWiFiCommissioningInstance.Init();
#else
	return CHIP_ERROR_INTERNAL;
#endif /* CONFIG_NET_L2_OPENTHREAD */

	mSwitch[0].Init(kOnOffRelayEndpointId_1, ONOFF_SWITCH_BUTTON_1);
	mSwitch[1].Init(kOnOffRelayEndpointId_2, ONOFF_SWITCH_BUTTON_2);
	mSwitch[2].Init(kOnOffRelayEndpointId_3, ONOFF_SWITCH_BUTTON_3);
	mSwitch[3].Init(kOnOffRelayEndpointId_4, ONOFF_SWITCH_BUTTON_4);
	BindingHandler::GetInstance().Init();

	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);
	RelayWidget::InitGpio();

	sOnOffLED_1.Init(ONOFF_SWITCH_LED_1);
	sOnOffLED_2.Init(ONOFF_SWITCH_LED_2);
	sOnOffLED_3.Init(ONOFF_SWITCH_LED_3);
	sOnOffLED_4.Init(ONOFF_SWITCH_LED_4);
	sStatusLED = &sOnOffLED_1;
	sIdentifyLED = &sOnOffLED_2;
	mSwitch[0].SetLED(&sOnOffLED_1);
	mSwitch[1].SetLED(&sOnOffLED_2);
	mSwitch[2].SetLED(&sOnOffLED_3);
	mSwitch[3].SetLED(&sOnOffLED_4);
#if defined (NRF52840_XXAA)
	mRelay[0].Init(kOnOffRelayEndpointId_1, 8);
	mRelay[1].Init(kOnOffRelayEndpointId_2, 7);
#if CONFIG_NUMBER_OF_RELAY == 4
	mRelay[2].Init(kOnOffRelayEndpointId_3, 6);
	mRelay[3].Init(kOnOffRelayEndpointId_4, 5);
#endif
#elif defined (NRF5340_XXAA)
	mRelay[0].Init(kOnOffRelayEndpointId_1, 9);
	mRelay[1].Init(kOnOffRelayEndpointId_2, 8);
#if CONFIG_NUMBER_OF_RELAY == 4
	mRelay[2].Init(kOnOffRelayEndpointId_3, 7);
	mRelay[3].Init(kOnOffRelayEndpointId_4, 6);
#endif
#endif
	mSwitch[0].SetRelay(&mRelay[0]);
	mSwitch[1].SetRelay(&mRelay[1]);
#if CONFIG_NUMBER_OF_RELAY == 4
	mSwitch[2].SetRelay(&mRelay[2]);
	mSwitch[3].SetRelay(&mRelay[3]);
#endif
	UpdateStatusLED();

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return chip::System::MapErrorZephyr(ret);
	}

	/* Initialize timers */
	k_timer_init(&sFunctionTimer, AppTask::FunctionTimerTimeoutCallback, nullptr);
	k_timer_user_data_set(&sFunctionTimer, this);

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
	/* Initialize DFU over SMP */
	GetDFUOverSMP().Init();
	GetDFUOverSMP().ConfirmNewImage();
#endif
#ifdef CONFIG_CHIP_NUS
	/* Initialize Nordic UART Service for Switch purposes */
	if (!GetNUSService().Init(kSwitchNUSPriority, kAdvertisingIntervalMin, kAdvertisingIntervalMax)) {
		ChipLogError(Zcl, "Cannot initialize NUS service");
	}
	GetNUSService().RegisterCommand("toggle 1", sizeof("toggle 1"), NUSToggle1Callback, nullptr);
	GetNUSService().RegisterCommand("toggle 2", sizeof("toggle 2"), NUSToggle2Callback, nullptr);
	GetNUSService().RegisterCommand("toggle 3", sizeof("toggle 3"), NUSToggle3Callback, nullptr);
	GetNUSService().RegisterCommand("toggle 4", sizeof("toggle 4"), NUSToggle4Callback, nullptr);
	GetNUSService().RegisterCommand("get 1", sizeof("get 1"), NUSGet1Callback, nullptr);
	GetNUSService().RegisterCommand("get 2", sizeof("get 2"), NUSGet2Callback, nullptr);
	GetNUSService().RegisterCommand("get 3", sizeof("get 3"), NUSGet3Callback, nullptr);
	GetNUSService().RegisterCommand("get 4", sizeof("get 4"), NUSGet4Callback, nullptr);
	if(!GetNUSService().StartServer()){
		LOG_ERR("GetNUSService().StartServer() failed");
	}
#endif

	/* Initialize CHIP server */
#if CONFIG_CHIP_FACTORY_DATA
	ReturnErrorOnFailure(mFactoryDataProvider.Init());
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	static CommonCaseDeviceServerInitParams initParams;
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

#ifdef CONFIG_CHIP_ICD_SUBSCRIPTION_HANDLING
	chip::app::InteractionModelEngine::GetInstance()->RegisterReadHandlerAppCallback(&GetICDUtil());
#endif

	/*
	 * Add CHIP event handler and start CHIP thread.
	 * Note that all the initialization code should happen prior to this point to avoid data races
	 * between the main and the CHIP threads.
	 */
	PlatformMgr().AddEventHandler(ChipEventHandler, 0);

	err = PlatformMgr().StartEventLoopTask();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
		return err;
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	AppEvent event = {};

	while (true) {
		k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
		DispatchEvent(event);
	}

	return CHIP_NO_ERROR;
}

void AppTask::LightingActionEventHandler(const AppEvent &event)
{
	if (event.ButtonEvent.PinNo == ONOFF_SWITCH_BUTTON_1 || event.ButtonEvent.PinNo == ONOFF_SWITCH_BUTTON_2 || event.ButtonEvent.PinNo == ONOFF_SWITCH_BUTTON_3 || event.ButtonEvent.PinNo == ONOFF_SWITCH_BUTTON_4) {
		LightSwitch *lightSwitch = Instance().GetSwitchByPin(event.ButtonEvent.PinNo);
		if (lightSwitch) {
			if (lightSwitch->GetLED()) {
				lightSwitch->GetLED()->Invert();
				if (Instance().GetSwitchByPin(event.ButtonEvent.PinNo)) {
					Instance().UpdateClusterState(Instance().GetSwitchByPin(event.ButtonEvent.PinNo)->GetLightSwitchEndpointId());
				}
			}
		}
		return;
	}
}


void AppTask::ButtonPushHandler(const AppEvent &event)
{
	if (event.Type == AppEventType::Button) {
		switch (event.ButtonEvent.PinNo) {
		case FUNCTION_BUTTON:
			Instance().StartTimer(Timer::Function, kFactoryResetTriggerTimeout);
			Instance().mFunction = FunctionEvent::SoftwareUpdate;
			break;
		default:
			break;
		}
	}
}

void AppTask::ButtonReleaseHandler(const AppEvent &event)
{
	AppEvent button_event;
	if (event.Type == AppEventType::Button) {
		switch (event.ButtonEvent.PinNo) {
		case FUNCTION_BUTTON:
			if (Instance().mFunction == FunctionEvent::SoftwareUpdate) {
				Instance().CancelTimer(Timer::Function);
				Instance().mFunction = FunctionEvent::NoneSelected;

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
				GetDFUOverSMP().StartServer();
				UpdateStatusLED();
#else
				LOG_INF("Software update is disabled");
#endif
			} else if (Instance().mFunction == FunctionEvent::FactoryReset) {
				UpdateStatusLED();

				Instance().CancelTimer(Timer::Function);
				Instance().mFunction = FunctionEvent::NoneSelected;
				LOG_INF("Factory Reset has been canceled");
			}
			break;
		default:
			break;
		}
	}
}

void AppTask::TimerEventHandler(const AppEvent &event)
{
	if (event.Type == AppEventType::Timer) {
		switch (static_cast<Timer>(event.TimerEvent.TimerType)) {
		case Timer::Function:
			if (Instance().mFunction == FunctionEvent::SoftwareUpdate) {
				LOG_INF("Factory Reset has been triggered. Release button within %u ms to cancel.",
					kFactoryResetCancelWindowTimeout);
				Instance().StartTimer(Timer::Function, kFactoryResetCancelWindowTimeout);
				Instance().mFunction = FunctionEvent::FactoryReset;

#ifdef CONFIG_STATE_LEDS
				/* reset all LEDs to synchronize factory reset blinking */
				sStatusLED->Set(false);
				sIdentifyLED->Set(false);
#if NUMBER_OF_LEDS == 4
				sFactoryResetLEDs.Set(false);
#endif

				sStatusLED->Blink(LedConsts::kBlinkRate_ms);
				sIdentifyLED->Blink(LedConsts::kBlinkRate_ms);
#if NUMBER_OF_LEDS == 4
				sFactoryResetLEDs.Blink(LedConsts::kBlinkRate_ms);
#endif
#endif
			} else if (Instance().mFunction == FunctionEvent::FactoryReset) {
				Instance().mFunction = FunctionEvent::NoneSelected;
				LOG_INF("Factory Reset triggered");
				chip::Server::GetInstance().ScheduleFactoryReset();
			}
			break;
		default:
			break;
		}
	}
}

void AppTask::IdentifyStartHandler(Identify *)
{
	AppEvent event;
	event.Type = AppEventType::IdentifyStart;
	event.Handler = [](const AppEvent &) { sIdentifyLED->Blink(LedConsts::kIdentifyBlinkRate_ms); };
	PostEvent(event);
}

void AppTask::IdentifyStopHandler(Identify *)
{
	AppEvent event;
	event.Type = AppEventType::IdentifyStop;
	event.Handler = [](const AppEvent &) { sIdentifyLED->Set(false); };
	PostEvent(event);
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		UpdateStatusLED();
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
		if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started) {
			if (NFCMgr().IsTagEmulationStarted()) {
				LOG_INF("NFC Tag emulation is already started");
			} else {
				ShareQRCodeOverNFC(
					chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
			}
		} else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped) {
			NFCMgr().StopTagEmulation();
		}
#endif
		sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		UpdateStatusLED();
		break;
#if defined(CONFIG_NET_L2_OPENTHREAD)
	case DeviceEventType::kDnssdInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
		break;
	case DeviceEventType::kThreadStateChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsThreadProvisioned();
		sIsNetworkEnabled = ConnectivityMgr().IsThreadEnabled();
#elif defined(CONFIG_CHIP_WIFI)
	case DeviceEventType::kWiFiConnectivityChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsWiFiStationProvisioned();
		sIsNetworkEnabled = ConnectivityMgr().IsWiFiStationEnabled();
#if CONFIG_CHIP_OTA_REQUESTOR
		if (event->WiFiConnectivityChange.Result == kConnectivity_Established) {
			InitBasicOTARequestor();
		}
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
#endif
		UpdateStatusLED();
		break;
	case DeviceEventType::kBindingsChangedViaCluster:
	case DeviceEventType::kServerReady:
		{
			AppEvent bindingEvent;

			bindingEvent.Type = AppEventType::BindingChanged;
			bindingEvent.Handler = AppTask::BindingChangedEventHandler;
			PostEvent(bindingEvent);
		}
		break;
	default:
		break;
	}
}

void AppTask::UpdateStatusLED()
{
#ifdef CONFIG_STATE_LEDS
	/* Update the status LED.
	 *
	 * If IPv6 network and service provisioned, keep the LED on constantly.
	 *
	 * If the system has BLE connection(s) up till the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED for a very short time. */
	if (sIsNetworkProvisioned && sIsNetworkEnabled) {
		sStatusLED->Set(true);
	} else if (sHaveBLEConnections) {
		sStatusLED->Blink(LedConsts::StatusLed::Unprovisioned::kOn_ms,
				 LedConsts::StatusLed::Unprovisioned::kOff_ms);
	} else {
		sStatusLED->Blink(LedConsts::StatusLed::Provisioned::kOn_ms, LedConsts::StatusLed::Provisioned::kOff_ms);
	}
#endif
}

void AppTask::ButtonEventHandler(uint32_t buttonState, uint32_t hasChanged)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;
	uint32_t buttonMask;

	if (FUNCTION_BUTTON_MASK & buttonState & hasChanged) {
		buttonEvent.ButtonEvent.PinNo = FUNCTION_BUTTON;
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonPushed);
		buttonEvent.Handler = ButtonPushHandler;
		PostEvent(buttonEvent);
	} else if (FUNCTION_BUTTON_MASK & hasChanged) {
		buttonEvent.ButtonEvent.PinNo = FUNCTION_BUTTON;
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = ButtonReleaseHandler;
		PostEvent(buttonEvent);
	}
	buttonMask = ONOFF_SWITCH_BUTTON_1_MASK;
	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_1;

	if (ONOFF_SWITCH_BUTTON_1_MASK & buttonState & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_1 press");
	} else if (ONOFF_SWITCH_BUTTON_1_MASK & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_1 release");
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = LightingActionEventHandler;
		PostEvent(buttonEvent);
	}

	buttonMask = ONOFF_SWITCH_BUTTON_2_MASK;
	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_2;

	if (ONOFF_SWITCH_BUTTON_2_MASK & buttonState & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_2 press");
	} else if (ONOFF_SWITCH_BUTTON_2_MASK & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_2 release");
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = LightingActionEventHandler;
		PostEvent(buttonEvent);
	}

	buttonMask = ONOFF_SWITCH_BUTTON_3_MASK;
	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_3;

	if (ONOFF_SWITCH_BUTTON_3_MASK & buttonState & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_3 press");
	} else if (ONOFF_SWITCH_BUTTON_3_MASK & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_3 release");
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = LightingActionEventHandler;
		PostEvent(buttonEvent);
	}

	buttonMask = ONOFF_SWITCH_BUTTON_4_MASK;
	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_4;

	if (ONOFF_SWITCH_BUTTON_4_MASK & buttonState & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_4 press");
	} else if (ONOFF_SWITCH_BUTTON_4_MASK & hasChanged) {
		LOG_DBG("ONOFF_SWITCH_BUTTON_4 release");
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = LightingActionEventHandler;
		PostEvent(buttonEvent);
	}
}

void AppTask::StartTimer(Timer timer, uint32_t timeoutMs)
{
	switch (timer) {
	case Timer::Function:
		k_timer_start(&sFunctionTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	default:
		break;
	}
}

void AppTask::CancelTimer(Timer timer)
{
	switch (timer) {
	case Timer::Function:
		k_timer_stop(&sFunctionTimer);
		break;
	default:
		break;
	}
}

void AppTask::UpdateLedStateEventHandler(const AppEvent &event)
{
	if (event.Type == AppEventType::UpdateLedState) {
		event.UpdateLedStateEvent.LedWidget->UpdateState();
	}
}

void AppTask::BindingChangedEventHandler(const AppEvent &event)
{
	for (int i = 1; i < CONFIG_NUMBER_OF_SWITCH; i++) {
		Instance().GetSwitchByEndPoint(i)->SubscribeAttribute();
		k_sleep(K_MSEC(3000));
	}
}

void AppTask::LEDStateUpdateHandler(LEDWidget &aLedWidget)
{
	AppEvent event;
	event.Type = AppEventType::UpdateLedState;
	event.Handler = UpdateLedStateEventHandler;
	event.UpdateLedStateEvent.LedWidget = &aLedWidget;
	PostEvent(event);
}

void AppTask::FunctionTimerTimeoutCallback(k_timer *timer)
{
	if (!timer) {
		return;
	}

	AppEvent event;
	if (timer == &sFunctionTimer) {
		event.Type = AppEventType::Timer;
		event.TimerEvent.TimerType = (uint8_t)Timer::Function;
		event.TimerEvent.Context = k_timer_user_data_get(timer);
		event.Handler = TimerEventHandler;
		PostEvent(event);
	}
}

void AppTask::PostEvent(const AppEvent &event)
{
	if (k_msgq_put(&sAppEventQueue, &event, K_NO_WAIT) != 0) {
		LOG_INF("Failed to post event to app task event queue");
	}
}

void AppTask::DispatchEvent(const AppEvent &event)
{
	if (event.Handler) {
		event.Handler(event);
	} else {
		LOG_INF("Event received with no handler. Dropping event.");
	}
}

void AppTask::UpdateClusterState(chip::EndpointId aEndpointId)
{
	EmberAfStatus cluster_status;
	bool onoff;

	cluster_status = Clusters::OnOff::Attributes::OnOff::Get(aEndpointId, &onoff);
	if (cluster_status == EMBER_ZCL_STATUS_SUCCESS) {
		if (GetSwitchByEndPoint(aEndpointId)->GetLED()) {
			SystemLayer().ScheduleLambda([this, aEndpointId] {
				/* write the new on/off value */
				if (Instance().GetSwitchByEndPoint(aEndpointId)->GetLED()) {
					EmberAfStatus status =
						Clusters::OnOff::Attributes::OnOff::Set(aEndpointId, Instance().GetSwitchByEndPoint(aEndpointId)->GetLED()->Get());

					if (status != EMBER_ZCL_STATUS_SUCCESS) {
						LOG_ERR("Updating on/off cluster %d failed: %x", aEndpointId, status);
					}
				}
			});
			return;
		}
	} else {
		LOG_ERR("Get on/off cluster failed: %x", cluster_status);
	}
}

LightSwitch* AppTask::GetSwitchByEndPoint(chip::EndpointId aEndpointId)
{
	for (int i = 0; i < CONFIG_NUMBER_OF_SWITCH; i++) {
		if (mSwitch[i].GetLightSwitchEndpointId() == aEndpointId) {
			return &mSwitch[i];
		}
	}
	return nullptr;
}

LightSwitch* AppTask::GetSwitchByPin(uint32_t aGpioPin)
{
	for (int i = 0; i < CONFIG_NUMBER_OF_SWITCH; i++) {
		if (mSwitch[i].GetGpioPin() == aGpioPin) {
			return &mSwitch[i];
		}
	}
	return nullptr;
}

RelayWidget* AppTask::GetRelayByEndPoint(chip::EndpointId aEndpointId)
{
	for (int i = 0; i < CONFIG_NUMBER_OF_RELAY; i++) {
		if (mRelay[i].GetRelayEndpointId() == aEndpointId) {
			return &mRelay[i];
		}
	}
	return nullptr;
}

#ifdef CONFIG_CHIP_NUS
void AppTask::NUSToggle1Callback(void *context)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;

	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_1;
	buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
	buttonEvent.Handler = LightingActionEventHandler;
	PostEvent(buttonEvent);
}

void AppTask::NUSToggle2Callback(void *context)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;

	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_2;
	buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
	buttonEvent.Handler = LightingActionEventHandler;
	PostEvent(buttonEvent);
}

void AppTask::NUSToggle3Callback(void *context)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;

	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_3;
	buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
	buttonEvent.Handler = LightingActionEventHandler;
	PostEvent(buttonEvent);
}

void AppTask::NUSToggle4Callback(void *context)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;

	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_4;
	buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
	buttonEvent.Handler = LightingActionEventHandler;
	PostEvent(buttonEvent);
}

void AppTask::NUSGet1Callback(void *context)
{
	static char buffer[20];

	if (Instance().GetSwitchByPin(ONOFF_SWITCH_BUTTON_1)->GetLED()->Get()) {
		sprintf(buffer, "Relay is on");
	} else {
		sprintf(buffer, "Relay is off");
	}
	GetNUSService().SendData(buffer, sizeof(buffer));
}

void AppTask::NUSGet2Callback(void *context)
{
	static char buffer[20];

	if (Instance().GetSwitchByPin(ONOFF_SWITCH_BUTTON_2)->GetLED()->Get()) {
		sprintf(buffer, "Switch is on");
	} else {
		sprintf(buffer, "Switch is off");
	}
	GetNUSService().SendData(buffer, sizeof(buffer));
}

void AppTask::NUSGet3Callback(void *context)
{
	static char buffer[20];

	if (Instance().GetSwitchByPin(ONOFF_SWITCH_BUTTON_3)->GetLED()->Get()) {
		sprintf(buffer, "Switch is on");
	} else {
		sprintf(buffer, "RelSwitchy is off");
	}
	GetNUSService().SendData(buffer, sizeof(buffer));
}

void AppTask::NUSGet4Callback(void *context)
{
	static char buffer[20];

	if (Instance().GetSwitchByPin(ONOFF_SWITCH_BUTTON_4)->GetLED()->Get()) {
		sprintf(buffer, "Relay is on");
	} else {
		sprintf(buffer, "Relay is off");
	}
	GetNUSService().SendData(buffer, sizeof(buffer));
}
#endif