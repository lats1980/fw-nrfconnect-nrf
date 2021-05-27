/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include "slm_util.h"
#include "slm_at_hw_rev.h"
#include <device.h>
#include <drivers/adc.h>
#include <hal/nrf_saadc.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(slm_hw_rev, CONFIG_SLM_LOG_LEVEL);

/* ADC Settings */
#define ADC_RESOLUTION		12
#define ADC_GAIN		ADC_GAIN_1_4
#define ADC_REFERENCE		ADC_REF_VDD_1_4
#define ADC_ACQUISITION_TIME	ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10)
#define ADC_1ST_CHANNEL_ID	0
#define ADC_1ST_CHANNEL_INPUT	NRF_SAADC_INPUT_AIN1
#define ADC_GPIO_PIN		14
#define ADC_DEVICE_NAME		DT_LABEL(DT_INST(0, nordic_nrf_saadc))
#define ADC_BUFFER_SIZE  1

/* global functions defined in different resources */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_SLM_SOCKET_RX_MAX * 2];
static int16_t m_sample_buffer[ADC_BUFFER_SIZE];
static const struct device *adc_dev;
struct adc_sequence adc_seq;

static const struct adc_channel_cfg m_1st_channel_cfg = {
	.gain	     = ADC_GAIN,
	.reference	= ADC_REFERENCE,
	.acquisition_time = ADC_ACQUISITION_TIME,
	.channel_id       = ADC_1ST_CHANNEL_ID,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
	.input_positive   = ADC_1ST_CHANNEL_INPUT,
#endif
};

int decode_hw_rev(int16_t v1, int16_t v2)
{
	int code = -1, err = 0;
	int16_t v3 = 0;

	/* Step 4: verify R2 */
	if (v1 <= (v2 * 105) / 100) {
		/* R2 is not fitted. Discharge for 1 ms */
		nrf_saadc_disable(NRF_SAADC);
		nrf_saadc_channel_input_set(NRF_SAADC,
					    ADC_1ST_CHANNEL_ID,
					    NRF_SAADC_INPUT_DISABLED,
					    NRF_SAADC_INPUT_DISABLED);
		nrf_gpio_pin_clear(ADC_GPIO_PIN);
		nrf_gpio_cfg_output(ADC_GPIO_PIN);
		/* Wait capacitor discharge */
		k_sleep(K_MSEC(1));
		/* Prepare for SAADC sampling */
		nrf_gpio_cfg_default(ADC_GPIO_PIN);
		err = adc_channel_setup(adc_dev, &m_1st_channel_cfg);
		if (err) {
			LOG_ERR("ADC setup error: %d\n", err);
			return err;
		}

		err = adc_read(adc_dev, &adc_seq);
		if (err) {
			LOG_ERR("ADC 3rd read error: %d", err);
			return err;
		}
		v3 = m_sample_buffer[0];
		LOG_DBG("v3:%d", v3);
		/* Decode HW REV */
		if (v3 > 10 && v3 < 320) {
			code = 1;
		} else if (v3 > 330 && v3 < 1200) {
			code = 2;
		} else if (v3 > 1210 && v3 < 2400) {
			code = 3;
		} else if (v3 > 2410 && v3 < 3600) {
			code = 4;
		}
	} else {
		/* R2 is present. Decode HW REV */
		if ((v1 > 3751 && v1 < 4095) && (v2 > 3212 && v2 < 3921)) {
			code = 5;
		} else if ((v1 > 3413 && v1 < 3750) && (v2 > 2921 && v2 < 3211)) {
			code = 6;
		} else if ((v1 > 3073 && v1 < 3412) && (v2 > 2629 && v2 < 2920)) {
			code = 7;
		} else if ((v1 > 2725 && v1 < 3072) && (v2 > 2330 && v2 < 2628)) {
			code = 8;
		} else if ((v1 > 2382 && v1 < 2724) && (v2 > 2037 && v2 < 2329)) {
			code = 9;
		} else if ((v1 > 2039 && v1 < 2381) && (v2 > 1743 && v2 < 2036)) {
			code = 10;
		} else if ((v1 > 1695 && v1 < 2038) && (v2 > 1448 && v2 < 1742)) {
			code = 11;
		} else if ((v1 > 1348 && v1 < 1694) && (v2 > 1151 && v2 < 1447)) {
			code = 12;
		} else if ((v1 > 1005 && v1 < 1347) && (v2 > 857 && v2 < 1150)) {
			code = 13;
		} else if ((v1 > 592 && v1 < 1004) && (v2 > 345 && v2 < 856)) {
			code = 14;
		}
	}
	LOG_DBG("v1:%d v2:%d", v1, v2);

	return code;
}

int do_hw_rev_read(void)
{
	int err = 0, code = 0;
	int16_t v1 = 0, v2 = 0;

	adc_seq.channels	= BIT(ADC_1ST_CHANNEL_ID);
	adc_seq.buffer		= m_sample_buffer;
	adc_seq.buffer_size	= sizeof(m_sample_buffer);
	adc_seq.resolution	= ADC_RESOLUTION;

	/* Step 1: Charge up the C1 capacitor */
	nrf_gpio_pin_set(ADC_GPIO_PIN);
	nrf_gpio_cfg_output(ADC_GPIO_PIN);
	k_sleep(K_MSEC(50));

	/* Step 2: Sample 1st SAADC */
	nrf_gpio_cfg_default(ADC_GPIO_PIN);
	err = adc_channel_setup(adc_dev, &m_1st_channel_cfg);
	if (err) {
		LOG_ERR("ADC setup error: %d\n", err);
		return err;
	}
	err = adc_read(adc_dev, &adc_seq);
	if (err) {
		LOG_ERR("ADC 1st read error: %d", err);
		return err;
	}
	v1 = m_sample_buffer[0];

	/* Step 3: Wait capacitor discharge and sample 2nd SAADC */
	k_sleep(K_MSEC(1));
	err = adc_read(adc_dev, &adc_seq);
	if (err) {
		LOG_ERR("ADC 2nd read error: %d", err);
		return err;
	}
	v2 = m_sample_buffer[0];

	code = decode_hw_rev(v1, v2);
	LOG_DBG("Decoded HW revision: %d", code);
	nrf_saadc_disable(NRF_SAADC);
	nrf_saadc_channel_input_set(NRF_SAADC,
				    ADC_1ST_CHANNEL_ID,
				    NRF_SAADC_INPUT_DISABLED,
				    NRF_SAADC_INPUT_DISABLED);
	sprintf(rsp_buf, "\r\n#XSLMHWREV: %d\r\n", code);
	rsp_send(rsp_buf, strlen(rsp_buf));

	return err;
}

/**@brief handle AT#XSLMHWREV commands
 *  AT#XSLMHWREV
 *  AT#XSLMHWREV? READ command not supported
 *  AT#XSLMHWREV=?
 */
int handle_at_hw_rev(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = do_hw_rev_read();
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XSLMHWREV: (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14)\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

int slm_at_hw_rev_init(void)
{
	int err = 0;

	adc_dev = device_get_binding(ADC_DEVICE_NAME);
	if (!adc_dev) {
		LOG_ERR("ADC bind error");
		err = -EIO;
	}

	return err;
}

int slm_at_hw_rev_uninit(void)
{
	int err = 0;

	return err;
}
