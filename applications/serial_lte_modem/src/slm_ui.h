/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file
 *
 * @brief   User interface module for serial LTE modem.
 *
 * Module that handles user interaction through LEDs.
 */

#ifndef SLM_UI_H__
#define SLM_UI_H__

#include <zephyr.h>
#include <drivers/gpio.h>

#define RSRP_THRESHOLD_1			20
#define RSRP_THRESHOLD_2			40
#define RSRP_THRESHOLD_3			60
#define RSRP_THRESHOLD_4			80

/* UI LEDs state list */
enum ui_leds_state {
	UI_LEDS_OFF,
	UI_LEDS_ON
};

/* LED state list */
enum ui_led_state {
	UI_MUTE,
	UI_UNMUTE,
	UI_LTE_DISCONNECTED,
	UI_LTE_CONNECTING,
	UI_LTE_CONNECTED,
	UI_DATA_NONE,
	UI_DATA_SLOW,
	UI_DATA_NORMAL,
	UI_DATA_FAST,
	UI_SIGNAL_OFF,
	UI_SIGNAL_L0,
	UI_SIGNAL_L1,
	UI_SIGNAL_L2,
	UI_SIGNAL_L3,
	UI_SIGNAL_L4,
#if defined(CONFIG_SLM_UI_DIAG)
	UI_DIAG_OFF,
	UI_DIAG_ON,
#endif
#if defined(CONFIG_SLM_MOD_FLASH)
	UI_ONLINE_OFF,
	UI_ONLINE_IDLE,
	UI_ONLINE_CONNECTED,
#endif
	LED_LTE_STATE_COUNT
};

/* LED ID List */
enum led_id {
#if defined(CONFIG_SLM_UI_LTE_STATE)
	LED_ID_LTE,
#endif
#if defined(CONFIG_SLM_UI_DATA_ACTIVITY)
	LED_ID_DATA,
#endif
#if defined(CONFIG_SLM_UI_LTE_SIGNAL)
	LED_ID_SIGNAL,
#endif
#if defined(CONFIG_SLM_UI_DIAG)
	LED_ID_DIAG,
#endif
#if defined(CONFIG_SLM_MOD_FLASH)
	LED_ID_MOD_LED,
#endif
	LED_ID_COUNT
};

struct led_effect_step {
	bool led_on;
	uint16_t substep_cnt;
	uint16_t substep_time;
};

struct led_effect {
	struct led_effect_step *steps;
	uint16_t step_cnt;
	uint16_t loop_cnt;
};

struct led {
	uint16_t fn;
	enum ui_led_state state;
	const struct led_effect *effect;
	uint16_t effect_step;
	uint16_t effect_substep;
	uint16_t effect_loop;
	struct k_delayed_work work;
};

/* LED EFFECTS Definition */
#define LED_EFFECT_LED_ON()				\
	{						\
		.steps = ((struct led_effect_step[]) {	\
			{				\
				.led_on = true,		\
				.substep_cnt = 1,	\
				.substep_time = 0,	\
			},				\
		}),					\
		.step_cnt = 1,				\
		.loop_cnt = 1,				\
	}


#define LED_EFFECT_LED_OFF()				\
	{						\
		.steps = ((struct led_effect_step[]) {	\
			{				\
				.led_on = false,	\
				.substep_cnt = 1,	\
				.substep_time = 0,	\
			},				\
		}),					\
		.step_cnt = 1,				\
		.loop_cnt = 1,				\
	}

#define LED_EFFECT_LED_BLINK(_period, _loop_cnt)		\
	{							\
		.steps = ((struct led_effect_step[]) {		\
			{					\
				.led_on = true,			\
				.substep_cnt = 1,		\
				.substep_time = (_period),	\
			},					\
			{					\
				.led_on = false,		\
				.substep_cnt = 1,		\
				.substep_time = (_period),	\
			},					\
		}),						\
		.step_cnt = 2,					\
		.loop_cnt = _loop_cnt,				\
	}

static const struct led_effect led_effect_list[LED_LTE_STATE_COUNT] = {
	[UI_MUTE]		= LED_EFFECT_LED_OFF(),
	[UI_UNMUTE]		= LED_EFFECT_LED_OFF(),
	[UI_LTE_DISCONNECTED]	= LED_EFFECT_LED_OFF(),
	[UI_LTE_CONNECTING]	= LED_EFFECT_LED_BLINK(500, 0),
	[UI_LTE_CONNECTED]	= LED_EFFECT_LED_ON(),
	[UI_DATA_NONE]		= LED_EFFECT_LED_OFF(),
	[UI_DATA_SLOW]		= LED_EFFECT_LED_BLINK(50, 1),
	[UI_DATA_NORMAL]	= LED_EFFECT_LED_BLINK(50, 3),
	[UI_DATA_FAST]		= LED_EFFECT_LED_BLINK(50, 5),
	[UI_SIGNAL_OFF]		= LED_EFFECT_LED_OFF(),
	[UI_SIGNAL_L0]		= LED_EFFECT_LED_BLINK(1000, 0),
	[UI_SIGNAL_L1]		= LED_EFFECT_LED_BLINK(1000, 0),
	[UI_SIGNAL_L2]		= LED_EFFECT_LED_BLINK(200, 0),
	[UI_SIGNAL_L3]		= LED_EFFECT_LED_BLINK(200, 0),
	[UI_SIGNAL_L4]		= LED_EFFECT_LED_BLINK(200, 0),
#if defined(CONFIG_SLM_UI_DIAG)
	[UI_DIAG_OFF]		= LED_EFFECT_LED_OFF(),
	[UI_DIAG_ON]		= LED_EFFECT_LED_ON(),
#endif
#if defined(CONFIG_SLM_MOD_FLASH)
	[UI_ONLINE_OFF]		= LED_EFFECT_LED_OFF(),
	[UI_ONLINE_IDLE]	= LED_EFFECT_LED_BLINK(250, 0),
	[UI_ONLINE_CONNECTED]	= LED_EFFECT_LED_ON(),
#endif
};

/**
 * @brief Initializes the user interface module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_ui_init(void);

/**
 * @brief Un-initializes the user interface module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_ui_uninit(void);

/**
 * @brief Set UI state on all LEDs.
 */
int slm_ui_set(enum ui_leds_state state);

/**
 * @brief Sets LED effect based in UI LED state.
 */
void ui_led_set_state(enum led_id, enum ui_led_state state);

#endif /* SLM_UI_H__ */
