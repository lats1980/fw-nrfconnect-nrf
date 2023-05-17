/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "generic_switch.h"
#include "battery.h"
#include "buzzer.h"
#include "led_widget.h"
#include <platform/CHIPDeviceLayer.h>

#include <DeviceInfoProviderImpl.h>

#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/ota-requestor/OTATestEventTriggerDelegate.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <dk_buttons_and_leds.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <ei_wrapper.h>

using namespace ::chip;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app;

LOG_MODULE_DECLARE(app);

namespace
{
enum class FunctionTimerMode { kDisabled, kFactoryResetTrigger, kFactoryResetComplete };
enum class LedState { kAlive, kAdvertisingBle, kConnectedBle, kProvisioned };

#if CONFIG_AVERAGE_CURRENT_CONSUMPTION <= 0
#error Invalid CONFIG_AVERAGE_CURRENT_CONSUMPTION value set
#endif

constexpr size_t kAppEventQueueSize = 10;
constexpr size_t kFactoryResetTriggerTimeoutMs = 3000;
constexpr size_t kFactoryResetCompleteTimeoutMs = 3000;
constexpr size_t kMeasurementsIntervalMs = 3000;
constexpr uint8_t kTemperatureMeasurementEndpointId = 1;
constexpr int16_t kTemperatureMeasurementAttributeMaxValue = 0x7fff;
constexpr int16_t kTemperatureMeasurementAttributeMinValue = 0x954d;
constexpr int16_t kTemperatureMeasurementAttributeInvalidValue = 0x8000;
constexpr uint8_t kHumidityMeasurementEndpointId = 2;
constexpr uint16_t kHumidityMeasurementAttributeMaxValue = 0x2710;
constexpr uint16_t kHumidityMeasurementAttributeMinValue = 0;
constexpr uint16_t kHumidityMeasurementAttributeInvalidValue = 0xffff;
constexpr uint8_t kPressureMeasurementEndpointId = 3;
constexpr int16_t kPressureMeasurementAttributeMaxValue = 0x7fff;
constexpr int16_t kPressureMeasurementAttributeMinValue = 0x8001;
constexpr int16_t kPressureMeasurementAttributeInvalidValue = 0x8000;
constexpr uint8_t kPowerSourceEndpointId = 0;
constexpr int16_t kMinimalOperatingVoltageMv = 3200;
constexpr int16_t kMaximalOperatingVoltageMv = 4050;
constexpr int16_t kWarningThresholdVoltageMv = 3450;
constexpr int16_t kCriticalThresholdVoltageMv = 3250;
constexpr uint8_t kMinBatteryPercentage = 0;
constexpr size_t kXYZMeasurementsIntervalMs = 16;
//constexpr size_t kXYZMeasurementsIntervalMs = 10;
constexpr EndpointId kGenericSwitchEndpointId = 4;
/* Value is expressed in half percent units ranging from 0 to 200. */
constexpr uint8_t kMaxBatteryPercentage = 200;
/* Battery capacity in uAh */
constexpr uint32_t kBatteryCapacityUaH = 1350000;
/* Average device current consumption in uA */
constexpr uint32_t kDeviceAverageCurrentConsumptionUa = CONFIG_AVERAGE_CURRENT_CONSUMPTION;
/* Fully charged battery operation time in seconds */
constexpr uint32_t kFullBatteryOperationTime = kBatteryCapacityUaH / kDeviceAverageCurrentConsumptionUa * 3600;
/* It is recommended to toggle the signalled state with 0.5 s interval. */
constexpr size_t kIdentifyTimerIntervalMs = 500;

K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
k_timer sFunctionTimer;
k_timer sMeasurementsTimer;
k_timer sXYZMeasurementsTimer;
k_timer sIdentifyTimer;
FunctionTimerMode sFunctionTimerMode = FunctionTimerMode::kDisabled;

enum {
	ML_DROP_RESULT			= BIT(0),
	ML_CLEANUP_REQUIRED		= BIT(1),
	ML_FIRST_PREDICTION		= BIT(2),
	ML_RUNNING			= BIT(3),
};

uint8_t ml_control;

LEDWidget sRedLED;
LEDWidget sGreenLED;
LEDWidget sBlueLED;

bool sIsThreadProvisioned;
bool sIsThreadEnabled;
bool sIsBleAdvertisingEnabled;
bool sHaveBLEConnections;

/* NOTE! This key is for test/certification only and should not be available in production devices!
 * If CONFIG_CHIP_FACTORY_DATA is enabled, this value is read from the factory data.
 */
uint8_t sTestEventTriggerEnableKey[TestEventTriggerDelegate::kEnableKeyLength] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
										   0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
										   0xcc, 0xdd, 0xee, 0xff };
chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;
LedState sLedState = LedState::kAlive;

/* Add identify for all endpoints */
Identify sIdentifyTemperature = { chip::EndpointId{ kTemperatureMeasurementEndpointId }, AppTask::OnIdentifyStart,
				  AppTask::OnIdentifyStop, EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_AUDIBLE_BEEP };
Identify sIdentifyHumidity = { chip::EndpointId{ kHumidityMeasurementEndpointId }, AppTask::OnIdentifyStart,
			       AppTask::OnIdentifyStop, EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_AUDIBLE_BEEP };
Identify sIdentifyPressure = { chip::EndpointId{ kPressureMeasurementEndpointId }, AppTask::OnIdentifyStart,
			       AppTask::OnIdentifyStop, EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_AUDIBLE_BEEP };

const device *sBme688SensorDev = DEVICE_DT_GET_ONE(bosch_bme680);
const device *sAdxl362SensorDev = DEVICE_DT_GET_ONE(adi_adxl362);
} /* namespace */

AppTask AppTask::sAppTask;

static int buf_cleanup(void)
{
	bool cancelled = false;
	int err = ei_wrapper_clear_data(&cancelled);

	if (!err) {
		if (cancelled) {
			ml_control &= ~ML_RUNNING;
		}

		if (ml_control & ML_RUNNING) {
			ml_control |= ML_DROP_RESULT;
		}

		ml_control &= ~ML_CLEANUP_REQUIRED;
		ml_control |= ML_FIRST_PREDICTION;
	} else if (err == -EBUSY) {
		//__ASSERT_NO_MSG(ml_control & ML_RUNNING);
		LOG_ERR("Cannot cleanup buffer (err: %d)", err);
		ml_control |= ML_DROP_RESULT;
		ml_control |= ML_CLEANUP_REQUIRED;
	} else {
		LOG_ERR("Cannot cleanup buffer (err: %d)", err);
		//report_error();
	}

	return err;
}

static void start_prediction(void)
{
	int err;
	size_t window_shift;
	size_t frame_shift;

	if (ml_control & ML_RUNNING) {
		return;
	}

	if (ml_control & ML_CLEANUP_REQUIRED) {
		err = buf_cleanup();
		if (err) {
			return;
		}
	}

	if (ml_control & ML_FIRST_PREDICTION) {
		window_shift = 0;
		frame_shift = 0;
	} else {
		//window_shift = SHIFT_WINDOWS;
		//frame_shift = SHIFT_FRAMES;
		window_shift = 1;
		frame_shift = 0;
	}

	err = ei_wrapper_start_prediction(window_shift, frame_shift);

	if (!err) {
		ml_control |= ML_RUNNING;
		ml_control &= ~ML_FIRST_PREDICTION;
	} else {
		LOG_ERR("Cannot start prediction (err: %d)", err);
		//report_error();
	}
}

void AppTask::result_ready_cb(int err)
{
	LOG_ERR("Result ready callback(err: %d)", err);


	//bool drop_result = (err) || (ml_control & ML_DROP_RESULT) || (state == STATE_ERROR);
	bool drop_result = (err) || (ml_control & ML_DROP_RESULT);
	static int old_result = -1, new_result;

	if (err) {
		LOG_ERR("Result ready callback returned error (err: %d)", err);
		//report_error();
	} else {
		ml_control &= ~ML_DROP_RESULT;
		ml_control &= ~ML_RUNNING;

		//if (state == STATE_ACTIVE) {
			start_prediction();
		//}
	}

	if (!drop_result) {
		int ret;
		const char *label;
		float value;
		//float prev_value;
		size_t idx;
		//size_t prev_idx;
		float anomaly;

		ret = ei_wrapper_get_next_classification_result(&label, &value, &idx);
		if (!ret) {
			if (!label) {
				LOG_ERR("Returned label is NULL");
				return;
			}
			LOG_INF("%s, %f", label, value);
			if (strncmp(label, "Normal", 6) == 0) {
				LOG_INF("Normal");
				new_result = 1;
				if (old_result == new_result) {
					sAppTask.PostEvent(AppEvent{ AppEvent::MLResultNormal });
				}
			} else if (strncmp(label, "Unbala", 6) == 0) {
				LOG_INF("Unbalance");
				new_result = 2;
				if (old_result == new_result) {
					sAppTask.PostEvent(AppEvent{ AppEvent::MLResultUnbalance });
				}
			} else {
				LOG_INF("Ignore result");
				new_result = 3;
			}
			old_result = new_result;
		} else {
			ret = ei_wrapper_get_anomaly(&anomaly);
			if (!ret) {
				LOG_INF("anomaly:%f", anomaly);
			} else {
				LOG_ERR("Fail to retrieve anomaly");
			}
		}

	}
}

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

	err = ThreadStackMgr().InitThreadStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed");
		return err;
	}

#ifdef CONFIG_OPENTHREAD_MTD_SED
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#else
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
#endif
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed");
		return err;
	}

	GenericSwitch::GetInstance().Init(kGenericSwitchEndpointId);

	/* Initialize RGB LED */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

	sRedLED.Init(DK_LED1);
	sGreenLED.Init(DK_LED2);
	sBlueLED.Init(DK_LED3);

	UpdateStatusLED();

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonStateHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return chip::System::MapErrorZephyr(ret);
	}

	if (!device_is_ready(sBme688SensorDev)) {
		LOG_ERR("BME688 sensor device not ready");
		return chip::System::MapErrorZephyr(-ENODEV);
	}

	if (!device_is_ready(sAdxl362SensorDev)) {
		LOG_ERR("ADXL362 sensor device not ready");
		return chip::System::MapErrorZephyr(-ENODEV);
	}

	ml_control |= ML_FIRST_PREDICTION;
	ret = ei_wrapper_init(result_ready_cb);
	if (ret) {
		LOG_ERR("ei_wrapper_init() failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BatteryMeasurementInit();
	if (ret) {
		LOG_ERR("Battery measurement init failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BatteryMeasurementEnable();
	if (ret) {
		LOG_ERR("Enabling battery measurement failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BatteryChargeControlInit();
	if (ret) {
		LOG_ERR("Battery charge control init failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BuzzerInit();
	if (ret) {
		LOG_ERR("Buzzer init failed");
		return chip::System::MapErrorZephyr(ret);
	}

/* Get factory data */
#ifdef CONFIG_CHIP_FACTORY_DATA
	ReturnErrorOnFailure(mFactoryDataProvider.Init());
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
	/* Read EnableKey from the factory data. */
	MutableByteSpan enableKey(sTestEventTriggerEnableKey);
	err = mFactoryDataProvider.GetEnableKey(enableKey);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("mFactoryDataProvider.GetEnableKey() failed. Could not delegate a test event trigger");
		memset(sTestEventTriggerEnableKey, 0, sizeof(sTestEventTriggerEnableKey));
	}
#else
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

#ifdef CONFIG_MCUMGR_SMP_BT
	/* Initialize DFU over SMP */
	GetDFUOverSMP().Init();
	GetDFUOverSMP().ConfirmNewImage();
	GetDFUOverSMP().StartServer();
#endif

	/* Initialize timers */
	k_timer_init(
		&sFunctionTimer, [](k_timer *) { sAppTask.PostEvent(AppEvent{ AppEvent::FunctionTimer }); }, nullptr);
	k_timer_init(
		&sMeasurementsTimer, [](k_timer *) { sAppTask.PostEvent(AppEvent{ AppEvent::MeasurementsTimer }); },
		nullptr);
	k_timer_start(&sMeasurementsTimer, K_MSEC(kMeasurementsIntervalMs), K_MSEC(kMeasurementsIntervalMs));
	k_timer_init(
		&sIdentifyTimer, [](k_timer *) { sAppTask.PostEvent(AppEvent{ AppEvent::IdentifyTimer }); }, nullptr);
	k_timer_init(
		&sXYZMeasurementsTimer, [](k_timer *) { sAppTask.PostEvent(AppEvent{ AppEvent::XYZMeasurementsTimer }); },
		nullptr);
	k_timer_start(&sXYZMeasurementsTimer, K_MSEC(kXYZMeasurementsIntervalMs), K_MSEC(kXYZMeasurementsIntervalMs));
	start_prediction();
	/*
	ret = ei_wrapper_start_prediction(1, 0);
	if (ret) {
		LOG_ERR("Buzzer init failed");
		return chip::System::MapErrorZephyr(ret);
	}
	*/

	/* Initialize CHIP server */
	static chip::CommonCaseDeviceServerInitParams initParams;
	static OTATestEventTriggerDelegate testEventTriggerDelegate{ ByteSpan(sTestEventTriggerEnableKey) };
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	initParams.testEventTriggerDelegate = &testEventTriggerDelegate;
	ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));

	gExampleDeviceInfoProvider.SetStorageDelegate(&Server::GetInstance().GetPersistentStorage());
	chip::DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

	/*
	 * Add CHIP event handler and start CHIP thread.
	 * Note that all the initialization code should happen prior to this point
	 * to avoid data races between the main and the CHIP threads.
	 */
	PlatformMgr().AddEventHandler(ChipEventHandler, 0);

	err = PlatformMgr().StartEventLoopTask();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
		return err;
	}

	return CHIP_NO_ERROR;
}

void AppTask::OpenPairingWindow()
{
	if (Server::GetInstance().GetFabricTable().FabricCount() != 0) {
		LOG_INF("Matter service BLE advertising not started - device is already commissioned");
		return;
	}

	if (ConnectivityMgr().IsBLEAdvertisingEnabled()) {
		LOG_INF("BLE advertising is already enabled");
		return;
	}

	if (chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() !=
	    CHIP_NO_ERROR) {
		LOG_ERR("OpenBasicCommissioningWindow() failed");
	}
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

void AppTask::PostEvent(const AppEvent &event)
{
	if (k_msgq_put(&sAppEventQueue, &event, K_NO_WAIT)) {
		LOG_ERR("Failed to post event to app task event queue");
	}
}

void AppTask::DispatchEvent(AppEvent &event)
{
	switch (event.Type) {
	case AppEvent::FunctionPress:
		ButtonPushHandler();
		break;
	case AppEvent::FunctionRelease:
		ButtonReleaseHandler();
		break;
	case AppEvent::FunctionTimer:
		FunctionTimerHandler();
		break;
	case AppEvent::MeasurementsTimer:
		MeasurementsTimerHandler();
		break;
	case AppEvent::XYZMeasurementsTimer:
		XYZMeasurementsTimerHandler();
		break;
	case AppEvent::IdentifyTimer:
		IdentifyTimerHandler();
		break;
	case AppEvent::UpdateLedState:
		event.UpdateLedStateEvent.LedWidget->UpdateState();
		break;
	case AppEvent::MLResultNormal:
		GenericSwitch::GetInstance().GenericSwitchShortPress();
		break;
	case AppEvent::MLResultUnbalance:
		GenericSwitch::GetInstance().GenericSwitchLongPress();
		break;
	default:
		LOG_INF("Unknown event received");
		break;
	}
}

void AppTask::ButtonPushHandler()
{
	sFunctionTimerMode = FunctionTimerMode::kFactoryResetTrigger;
	k_timer_start(&sFunctionTimer, K_MSEC(kFactoryResetTriggerTimeoutMs), K_NO_WAIT);
}

void AppTask::ButtonReleaseHandler()
{
	/* If the button was released within the first kFactoryResetTriggerTimeoutMs, open the BLE pairing
	 * window. */
	if (sFunctionTimerMode == FunctionTimerMode::kFactoryResetTrigger) {
		GetAppTask().OpenPairingWindow();
	}

	sFunctionTimerMode = FunctionTimerMode::kDisabled;
	k_timer_stop(&sFunctionTimer);
}

void AppTask::ButtonStateHandler(uint32_t buttonState, uint32_t hasChanged)
{
	if (hasChanged & DK_BTN1_MSK) {
		if (buttonState & DK_BTN1_MSK)
			sAppTask.PostEvent(AppEvent{ AppEvent::FunctionPress });
		else
			sAppTask.PostEvent(AppEvent{ AppEvent::FunctionRelease });
	}
}

void AppTask::FunctionTimerHandler()
{
	switch (sFunctionTimerMode) {
	case FunctionTimerMode::kFactoryResetTrigger:
		LOG_INF("Factory Reset triggered. Release button within %ums to cancel.",
			kFactoryResetCompleteTimeoutMs);
		sFunctionTimerMode = FunctionTimerMode::kFactoryResetComplete;
		k_timer_start(&sFunctionTimer, K_MSEC(kFactoryResetCompleteTimeoutMs), K_NO_WAIT);
		break;
	case FunctionTimerMode::kFactoryResetComplete:
		ConfigurationMgr().InitiateFactoryReset();
		break;
	default:
		break;
	}
}

void AppTask::MeasurementsTimerHandler()
{
	sAppTask.UpdateClustersState();
}

void AppTask::XYZMeasurementsTimerHandler()
{
	//const int result = sensor_sample_fetch(sAdxl362SensorDev);
	//struct sensor_value data[ACCELEROMETER_CHANNELS];
	struct sensor_value data[3];
	int ret;
	float float_data[3];

	ret = sensor_sample_fetch(sAdxl362SensorDev);
	if (ret != 0) {
		LOG_ERR("Fetching data from ADXL362 sensor failed with: %d", ret);
	} else {
		ret = sensor_channel_get(sAdxl362SensorDev, SENSOR_CHAN_ACCEL_XYZ, &data[0]);
		if (ret) {
			LOG_ERR("sensor_channel_get, error: %d", ret);
		} else {
			for (size_t i = 0; i < 3; i++) {
				float_data[i] = sensor_value_to_double(&data[i]);
			}
			ret = ei_wrapper_add_data(float_data, 3);
			if (ret) {
				LOG_ERR("Cannot add data for EI wrapper (err %d)", ret);
				//report_error();
				//return false;
			}
		}
	}
}

void AppTask::OnIdentifyStart(Identify *)
{
	k_timer_start(&sIdentifyTimer, K_MSEC(kIdentifyTimerIntervalMs), K_MSEC(kIdentifyTimerIntervalMs));
}

void AppTask::OnIdentifyStop(Identify *)
{
	k_timer_stop(&sIdentifyTimer);
	BuzzerSetState(false);
}

void AppTask::IdentifyTimerHandler()
{
	BuzzerToggleState();
}

void AppTask::UpdateTemperatureClusterState()
{
	struct sensor_value sTemperature;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_AMBIENT_TEMP, &sTemperature);
	if (result == 0) {
		/* Defined by cluster temperature measured value = 100 x temperature in degC with resolution of
		 * 0.01 degC. val1 is an integer part of the value and val2 is fractional part in one-millionth
		 * parts. To achieve resolution of 0.01 degC val2 needs to be divided by 10000. */
		int16_t newValue = static_cast<int16_t>(sTemperature.val1 * 100 + sTemperature.val2 / 10000);

		if (newValue > kTemperatureMeasurementAttributeMaxValue ||
		    newValue < kTemperatureMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kTemperatureMeasurementAttributeInvalidValue;
		}

		status = Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(
			kTemperatureMeasurementEndpointId, newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating temperature measurement %x", status);
		}
	} else {
		LOG_ERR("Getting temperature measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdatePressureClusterState()
{
	struct sensor_value sPressure;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_PRESS, &sPressure);
	if (result == 0) {
		/* Defined by cluster pressure measured value = 10 x pressure in kPa with resolution of 0.1 kPa.
		 * val1 is an integer part of the value and val2 is fractional part in one-millionth parts.
		 * To achieve resolution of 0.1 kPa val2 needs to be divided by 100000. */
		int16_t newValue = static_cast<int16_t>(sPressure.val1 * 10 + sPressure.val2 / 100000);

		if (newValue > kPressureMeasurementAttributeMaxValue ||
		    newValue < kPressureMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kPressureMeasurementAttributeInvalidValue;
		}

		status = Clusters::PressureMeasurement::Attributes::MeasuredValue::Set(kPressureMeasurementEndpointId,
										       newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating pressure measurement %x", status);
		}
	} else {
		LOG_ERR("Getting pressure measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdateRelativeHumidityClusterState()
{
	struct sensor_value sHumidity;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_HUMIDITY, &sHumidity);
	if (result == 0) {
		/* Defined by cluster humidity measured value = 100 x humidity in %RH with resolution of 0.01 %.
		 * val1 is an integer part of the value and val2 is fractional part in one-millionth parts.
		 * To achieve resolution of 0.01 % val2 needs to be divided by 10000. */
		uint16_t newValue = static_cast<int16_t>(sHumidity.val1 * 100 + sHumidity.val2 / 10000);

		if (newValue > kHumidityMeasurementAttributeMaxValue ||
		    newValue < kHumidityMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kHumidityMeasurementAttributeInvalidValue;
		}

		status = Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(
			kHumidityMeasurementEndpointId, newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating relative humidity measurement %x", status);
		}
	} else {
		LOG_ERR("Getting humidity measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdatePowerSourceClusterState()
{
	EmberAfStatus status;
	int32_t voltage = BatteryMeasurementReadVoltageMv();
	/* Value is expressed in half percent units ranging from 0 to 200. */
	uint8_t batteryPercentage;
	uint32_t batteryTimeRemaining;
	Clusters::PowerSource::PowerSourceStatus batteryStatus;
	Clusters::PowerSource::BatChargeLevel batteryChargeLevel;
	bool batteryPresent;
	Clusters::PowerSource::BatChargeState batteryCharged;

	if (voltage < 0) {
		voltage = 0;
		batteryPercentage = 0;
		batteryStatus = Clusters::PowerSource::PowerSourceStatus::kUnavailable;
		batteryPresent = false;

		LOG_ERR("Battery level measurement failed %d", voltage);
	} else {
		batteryStatus = Clusters::PowerSource::PowerSourceStatus::kActive;
		batteryPresent = true;
	}

	if (voltage <= kMinimalOperatingVoltageMv) {
		batteryPercentage = kMinBatteryPercentage;
	} else if (voltage >= kMaximalOperatingVoltageMv) {
		batteryPercentage = kMaxBatteryPercentage;
	} else {
		batteryPercentage = kMaxBatteryPercentage * (voltage - kMinimalOperatingVoltageMv) /
				    (kMaximalOperatingVoltageMv - kMinimalOperatingVoltageMv);
	}

	batteryTimeRemaining = kFullBatteryOperationTime * batteryPercentage / kMaxBatteryPercentage;

	if (voltage < kCriticalThresholdVoltageMv) {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevel::kCritical;
	} else if (voltage < kWarningThresholdVoltageMv) {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevel::kWarning;
	} else {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevel::kOk;
	}

	if (BatteryCharged()) {
		batteryCharged = Clusters::PowerSource::BatChargeState::kIsCharging;
	} else {
		batteryCharged = Clusters::PowerSource::BatChargeState::kIsNotCharging;
	}

	status = Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId, voltage);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery voltage failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, batteryPercentage);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery percentage failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatTimeRemaining::Set(kPowerSourceEndpointId, batteryTimeRemaining);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery time remaining failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatChargeLevel::Set(kPowerSourceEndpointId, batteryChargeLevel);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery charge level failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::Status::Set(kPowerSourceEndpointId, batteryStatus);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery status failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, batteryPresent);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery present failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatChargeState::Set(kPowerSourceEndpointId, batteryCharged);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery charge failed %x", status);
	}
}

void AppTask::UpdateClustersState()
{
	const int result = sensor_sample_fetch(sBme688SensorDev);

	if (result == 0) {
		UpdateTemperatureClusterState();
		UpdatePressureClusterState();
		UpdateRelativeHumidityClusterState();
	} else {
		LOG_ERR("Fetching data from BME688 sensor failed with: %d", result);
	}

	UpdatePowerSourceClusterState();
}

void AppTask::UpdateStatusLED()
{
	LedState nextState;

	if (sIsThreadProvisioned && sIsThreadEnabled) {
		nextState = LedState::kProvisioned;
	} else if (sHaveBLEConnections) {
		nextState = LedState::kConnectedBle;
	} else if (sIsBleAdvertisingEnabled) {
		nextState = LedState::kAdvertisingBle;
	} else {
		nextState = LedState::kAlive;
	}

	/* In case of changing signalled state, turn off all leds to synchronize blinking */
	if (nextState != sLedState) {
		sGreenLED.Set(false);
		sBlueLED.Set(false);
		sRedLED.Set(false);
	}
	sLedState = nextState;

	switch (sLedState) {
	case LedState::kAlive:
		sGreenLED.Blink(50, 950);
		break;
	case LedState::kAdvertisingBle:
		sBlueLED.Blink(50, 950);
		break;
	case LedState::kConnectedBle:
		sBlueLED.Blink(100, 100);
		break;
	case LedState::kProvisioned:
		sBlueLED.Blink(50, 950);
		sRedLED.Blink(50, 950);
		break;
	default:
		break;
	}
}

void AppTask::LEDStateUpdateHandler(LEDWidget &ledWidget)
{
	sAppTask.PostEvent(AppEvent{ AppEvent::UpdateLedState, &ledWidget });
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
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
		sIsBleAdvertisingEnabled = ConnectivityMgr().IsBLEAdvertisingEnabled();
		sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		UpdateStatusLED();
		break;
	case DeviceEventType::kThreadStateChange:
		sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
		sIsThreadEnabled = ConnectivityMgr().IsThreadEnabled();
		UpdateStatusLED();
		break;
	case DeviceEventType::kDnssdPlatformInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif
		break;
	default:
		break;
	}
}
