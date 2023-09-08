/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app_config.h"
#include "led_util.h"
#include "light_switch.h"
#include "pwm_device.h"
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
constexpr uint32_t kDimmerTriggeredTimeout = 500;
constexpr uint32_t kDimmerInterval = 300;
constexpr size_t kAppEventQueueSize = 10;
constexpr EndpointId kDimmerSwitchEndpointId_1 = 1;
constexpr EndpointId kDimmerSwitchEndpointId_2 = 2;
constexpr EndpointId kOnOffSwitchEndpointId_1 = 3;
constexpr EndpointId kOnOffSwitchEndpointId_2 = 4;
constexpr EndpointId kSpareSwitchEndpointId_1 = 5;
constexpr EndpointId kSpareSwitchEndpointId_2 = 6;
constexpr EndpointId kLightEndpointId = 1;
constexpr uint8_t kDefaultMinLevel = 0;
constexpr uint8_t kDefaultMaxLevel = 254;

#ifdef CONFIG_CHIP_NUS
constexpr uint16_t kAdvertisingIntervalMin = 400;
constexpr uint16_t kAdvertisingIntervalMax = 500;
constexpr uint8_t kSwitchNUSPriority = 2;
#endif
K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
k_timer sFunctionTimer;
k_timer sDimmerPressKeyTimer;
k_timer sDimmerTimer;

Identify sIdentify = { kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
		       EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_VISIBLE_LED };

LEDWidget sStatusLED;
LEDWidget sIdentifyLED;
LEDWidget sOnOffLED_1;
#if NUMBER_OF_LEDS == 4
FactoryResetLEDsWrapper<2> sFactoryResetLEDs{ { FACTORY_RESET_SIGNAL_LED, FACTORY_RESET_SIGNAL_LED1 } };
#endif

bool sIsNetworkProvisioned = false;
bool sIsNetworkEnabled = false;
bool sHaveBLEConnections = false;
bool sWasDimmerTriggered = false;

const struct pwm_dt_spec sLightPwmDevice = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led1));

// Define a custom attribute persister which makes actual write of the CurrentLevel attribute value
// to the non-volatile storage only when it has remained constant for 5 seconds. This is to reduce
// the flash wearout when the attribute changes frequently as a result of MoveToLevel command.
// DeferredAttribute object describes a deferred attribute, but also holds a buffer with a value to
// be written, so it must live so long as the DeferredAttributePersistenceProvider object.
DeferredAttribute gCurrentLevelPersister(ConcreteAttributePath(kLightEndpointId, Clusters::LevelControl::Id,
							       Clusters::LevelControl::Attributes::CurrentLevel::Id));
DeferredAttributePersistenceProvider gDeferredAttributePersister(Server::GetInstance().GetDefaultAttributePersister(),
								 Span<DeferredAttribute>(&gCurrentLevelPersister, 1),
								 System::Clock::Milliseconds32(5000));

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

	mSwitch[0].Init(kDimmerSwitchEndpointId_1, DIMMER_SWITCH_BUTTON_1);
	mSwitch[1].Init(kDimmerSwitchEndpointId_2, DIMMER_SWITCH_BUTTON_2);
	mSwitch[2].Init(kOnOffSwitchEndpointId_1, ONOFF_SWITCH_BUTTON_1);
	mSwitch[3].Init(kOnOffSwitchEndpointId_2, ONOFF_SWITCH_BUTTON_2);
	mSwitch[4].Init(kSpareSwitchEndpointId_1, SPARE_SWITCH_BUTTON_1);
	mSwitch[5].Init(kSpareSwitchEndpointId_2, SPARE_SWITCH_BUTTON_1);
	BindingHandler::GetInstance().Init();

	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

	sStatusLED.Init(SYSTEM_STATE_LED);
	sIdentifyLED.Init(IDENTIFY_LED);
	sOnOffLED_1.Init(ONOFF_SWITCH_LED_1);
	mSwitch[2].SetLED(&sOnOffLED_1);

	UpdateStatusLED();

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return chip::System::MapErrorZephyr(ret);
	}

	/* Initialize timers */
	k_timer_init(&sFunctionTimer, AppTask::FunctionTimerTimeoutCallback, nullptr);
	k_timer_init(&sDimmerPressKeyTimer, AppTask::FunctionTimerTimeoutCallback, nullptr);
	k_timer_init(&sDimmerTimer, AppTask::FunctionTimerTimeoutCallback, nullptr);
	k_timer_user_data_set(&sDimmerTimer, this);
	k_timer_user_data_set(&sDimmerPressKeyTimer, this);
	k_timer_user_data_set(&sFunctionTimer, this);

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
	/* Initialize DFU over SMP */
	GetDFUOverSMP().Init();
	GetDFUOverSMP().ConfirmNewImage();
#endif

	/* Initialize lighting device (PWM) */
	uint8_t minLightLevel = kDefaultMinLevel;
	Clusters::LevelControl::Attributes::MinLevel::Get(kLightEndpointId, &minLightLevel);

	uint8_t maxLightLevel = kDefaultMaxLevel;
	Clusters::LevelControl::Attributes::MaxLevel::Get(kLightEndpointId, &maxLightLevel);

	ret = mPWMDevice.Init(&sLightPwmDevice, minLightLevel, maxLightLevel, maxLightLevel);
	if (ret != 0) {
		return chip::System::MapErrorZephyr(ret);
	}
	mPWMDevice.SetCallbacks(ActionInitiated, ActionCompleted);

#ifdef CONFIG_CHIP_NUS
	/* Initialize Nordic UART Service for Switch purposes */
	if (!GetNUSService().Init(kSwitchNUSPriority, kAdvertisingIntervalMin, kAdvertisingIntervalMax)) {
		ChipLogError(Zcl, "Cannot initialize NUS service");
	}
	//GetNUSService().RegisterCommand("Lock", sizeof("Lock"), NUSLockCallback, nullptr);
	//GetNUSService().RegisterCommand("Unlock", sizeof("Unlock"), NUSUnlockCallback, nullptr);
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
	app::SetAttributePersistenceProvider(&gDeferredAttributePersister);
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
	PWMDevice::Action_t action = PWMDevice::INVALID_ACTION;
	int32_t actor = 0;

	if (event.ButtonEvent.PinNo == ONOFF_SWITCH_BUTTON_1) {
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

	if (event.Type == AppEventType::Lighting) {
		action = static_cast<PWMDevice::Action_t>(event.LightingEvent.Action);
		actor = event.LightingEvent.Actor;
	} else if (event.Type == AppEventType::Button) {
		action = Instance().mPWMDevice.IsTurnedOn() ? PWMDevice::OFF_ACTION : PWMDevice::ON_ACTION;
		actor = static_cast<int32_t>(AppEventType::Button);
	}

	if (action != PWMDevice::INVALID_ACTION && Instance().mPWMDevice.InitiateAction(action, actor, NULL)) {
		LOG_INF("Action is already in progress or active.");
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
		case
#if NUMBER_OF_BUTTONS == 2
			BLE_ADVERTISEMENT_START_AND_SWITCH_BUTTON:
			if (!ConnectivityMgr().IsBLEAdvertisingEnabled() &&
			    Server::GetInstance().GetFabricTable().FabricCount() == 0) {
				break;
			}
#else
			DIMMER_SWITCH_BUTTON_1:
#endif
			LOG_INF("Button has been pressed, keep in this state for at least 500 ms to change light sensitivity of binded lighting devices.");
			Instance().StartTimer(Timer::DimmerTrigger, kDimmerTriggeredTimeout);
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
#if NUMBER_OF_BUTTONS == 4
		case DIMMER_SWITCH_BUTTON_1:
#else
		case BLE_ADVERTISEMENT_START_AND_SWITCH_BUTTON:
			if (!ConnectivityMgr().IsBLEAdvertisingEnabled() &&
			    Server::GetInstance().GetFabricTable().FabricCount() == 0) {
				AppEvent buttonEvent;
				buttonEvent.Type = AppEventType::Button;
				buttonEvent.ButtonEvent.PinNo = BLE_ADVERTISEMENT_START_AND_SWITCH_BUTTON;
				buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonPushed);
				buttonEvent.Handler = StartBLEAdvertisementHandler;
				PostEvent(buttonEvent);
				break;
			}
#endif
			/* TODO: add multiple PWM instance and check dimmer status */
			if (!sWasDimmerTriggered) {
				if (Instance().GetSwitchByPin(event.ButtonEvent.PinNo)) {
					Instance().GetSwitchByPin(event.ButtonEvent.PinNo)->InitiateActionSwitch(LightSwitch::Action::Toggle);
				}
			}
			Instance().CancelTimer(Timer::Dimmer);
			Instance().CancelTimer(Timer::DimmerTrigger);
			sWasDimmerTriggered = false;

			button_event.Type = AppEventType::Button;
			button_event.ButtonEvent.PinNo = DIMMER_SWITCH_BUTTON_1;
			button_event.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonPushed);
			button_event.Handler = LightingActionEventHandler;
			PostEvent(button_event);
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
				sStatusLED.Set(false);
				sIdentifyLED.Set(false);
#if NUMBER_OF_LEDS == 4
				sFactoryResetLEDs.Set(false);
#endif

				sStatusLED.Blink(LedConsts::kBlinkRate_ms);
				sIdentifyLED.Blink(LedConsts::kBlinkRate_ms);
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
		case Timer::DimmerTrigger:
			LOG_INF("Dimming started...");
			sWasDimmerTriggered = true;
			Instance().GetSwitchByEndPoint(1)->InitiateActionSwitch(LightSwitch::Action::On);
			Instance().StartTimer(Timer::Dimmer, kDimmerInterval);
			Instance().CancelTimer(Timer::DimmerTrigger);
			break;
		case Timer::Dimmer:
			Instance().GetSwitchByEndPoint(1)->DimmerChangeBrightness();
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
	event.Handler = [](const AppEvent &) { sIdentifyLED.Blink(LedConsts::kIdentifyBlinkRate_ms); };
	PostEvent(event);
}

void AppTask::IdentifyStopHandler(Identify *)
{
	AppEvent event;
	event.Type = AppEventType::IdentifyStop;
	event.Handler = [](const AppEvent &) { sIdentifyLED.Set(false); };
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
#if NUMBER_OF_LEDS == 4
	sFactoryResetLEDs.Set(false);
#endif

	/* Update the status LED.
	 *
	 * If IPv6 network and service provisioned, keep the LED on constantly.
	 *
	 * If the system has BLE connection(s) up till the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED for a very short time. */
	if (sIsNetworkProvisioned && sIsNetworkEnabled) {
		sStatusLED.Set(true);
	} else if (sHaveBLEConnections) {
		sStatusLED.Blink(LedConsts::StatusLed::Unprovisioned::kOn_ms,
				 LedConsts::StatusLed::Unprovisioned::kOff_ms);
	} else {
		sStatusLED.Blink(LedConsts::StatusLed::Provisioned::kOn_ms, LedConsts::StatusLed::Provisioned::kOff_ms);
	}
#endif
}

void AppTask::ButtonEventHandler(uint32_t buttonState, uint32_t hasChanged)
{
	AppEvent buttonEvent;
	buttonEvent.Type = AppEventType::Button;

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

#if NUMBER_OF_BUTTONS == 2
	uint32_t buttonMask = BLE_ADVERTISEMENT_START_AND_SWITCH_BUTTON_MASK;
	buttonEvent.ButtonEvent.PinNo = BLE_ADVERTISEMENT_START_AND_SWITCH_BUTTON;
#else
	uint32_t buttonMask = DIMMER_SWITCH_BUTTON_1_MASK;
	buttonEvent.ButtonEvent.PinNo = DIMMER_SWITCH_BUTTON_1;
#endif

	if (buttonMask & buttonState & hasChanged) {
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonPushed);
		buttonEvent.Handler = ButtonPushHandler;
		PostEvent(buttonEvent);
	} else if (buttonMask & hasChanged) {
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>(AppEventType::ButtonReleased);
		buttonEvent.Handler = ButtonReleaseHandler;
		PostEvent(buttonEvent);
	}

	buttonMask = ONOFF_SWITCH_BUTTON_1_MASK;
	buttonEvent.ButtonEvent.PinNo = ONOFF_SWITCH_BUTTON_1;

	if (ONOFF_SWITCH_BUTTON_1_MASK & buttonState & hasChanged) {
		//LOG_DBG("ONOFF_SWITCH_BUTTON_1 press");
	} else if (ONOFF_SWITCH_BUTTON_1_MASK & hasChanged) {
		//LOG_DBG("ONOFF_SWITCH_BUTTON_1 release");
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
	case Timer::DimmerTrigger:
		k_timer_start(&sDimmerPressKeyTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::Dimmer:
		k_timer_start(&sDimmerTimer, K_MSEC(timeoutMs), K_MSEC(timeoutMs));
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
	case Timer::DimmerTrigger:
		k_timer_stop(&sDimmerPressKeyTimer);
		break;
	case Timer::Dimmer:
		k_timer_stop(&sDimmerTimer);
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

void AppTask::ActionInitiated(PWMDevice::Action_t action, int32_t actor)
{
	if (action == PWMDevice::ON_ACTION) {
		LOG_INF("Turn On Action has been initiated");
	} else if (action == PWMDevice::OFF_ACTION) {
		LOG_INF("Turn Off Action has been initiated");
	} else if (action == PWMDevice::LEVEL_ACTION) {
		LOG_INF("Level Action has been initiated");
	}
}

void AppTask::ActionCompleted(PWMDevice::Action_t action, int32_t actor)
{
	if (action == PWMDevice::ON_ACTION) {
		LOG_INF("Turn On Action has been completed");
	} else if (action == PWMDevice::OFF_ACTION) {
		LOG_INF("Turn Off Action has been completed");
	} else if (action == PWMDevice::LEVEL_ACTION) {
		LOG_INF("Level Action has been completed");
	}

	if (actor == static_cast<int32_t>(AppEventType::Button)) {
		/* TODO: enable 2nd PWM and use pin number as button event actor */
		Instance().UpdateClusterState(DIMMER_SWITCH_BUTTON_1);
	}
}

void AppTask::BindingChangedEventHandler(const AppEvent &event)
{
	for (int i = 1; i < NUMBER_OF_SWITCH; i++) {
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
	if (timer == &sDimmerPressKeyTimer) {
		event.Type = AppEventType::Timer;
		event.TimerEvent.TimerType = (uint8_t)Timer::DimmerTrigger;
		event.TimerEvent.Context = k_timer_user_data_get(timer);
		event.Handler = TimerEventHandler;
		PostEvent(event);
	}
	if (timer == &sDimmerTimer) {
		event.Type = AppEventType::Timer;
		event.TimerEvent.TimerType = (uint8_t)Timer::Dimmer;
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
	app::DataModel::Nullable<uint8_t> level;

	cluster_status = Clusters::OnOff::Attributes::OnOff::Get(aEndpointId, &onoff);
	if (cluster_status == EMBER_ZCL_STATUS_SUCCESS) {
		if (GetSwitchByEndPoint(aEndpointId)->GetLED()) {
			SystemLayer().ScheduleLambda([this, aEndpointId] {
				/* write the new on/off value */
				if (Instance().GetSwitchByEndPoint(aEndpointId)->GetLED()) {
					EmberAfStatus status =
						Clusters::OnOff::Attributes::OnOff::Set(aEndpointId, Instance().GetSwitchByEndPoint(aEndpointId)->GetLED()->Get());

					if (status != EMBER_ZCL_STATUS_SUCCESS) {
						LOG_ERR("Updating on/off cluster failed: %x", status);
					}
				}
			});
			return;
		}
	}
	cluster_status = Clusters::LevelControl::Attributes::CurrentLevel::Get(aEndpointId, level);
	if (cluster_status == EMBER_ZCL_STATUS_SUCCESS) {
		SystemLayer().ScheduleLambda([this, aEndpointId] {
			/* write the new on/off value */
			EmberAfStatus status =
				Clusters::OnOff::Attributes::OnOff::Set(aEndpointId, mPWMDevice.IsTurnedOn());
			if (status != EMBER_ZCL_STATUS_SUCCESS) {
				LOG_ERR("Updating on/off cluster failed: %x", status);
			}
			/* write the current level */
			status = Clusters::LevelControl::Attributes::CurrentLevel::Set(aEndpointId, mPWMDevice.GetLevel());
			if (status != EMBER_ZCL_STATUS_SUCCESS) {
				LOG_ERR("Updating level cluster failed: %x", status);
			}
		});
	}
}

LightSwitch* AppTask::GetSwitchByEndPoint(chip::EndpointId aEndpointId)
{
	for (int i = 0; i < NUMBER_OF_SWITCH; i++) {
		if (mSwitch[i].GetLightSwitchEndpointId() == aEndpointId) {
			return &mSwitch[i];
		}
	}
	return nullptr;
}

LightSwitch* AppTask::GetSwitchByPin(uint32_t aGpioPin)
{
	for (int i = 0; i < NUMBER_OF_SWITCH; i++) {
		if (mSwitch[i].GetGpioPin() == aGpioPin) {
			return &mSwitch[i];
		}
	}
	return nullptr;
}