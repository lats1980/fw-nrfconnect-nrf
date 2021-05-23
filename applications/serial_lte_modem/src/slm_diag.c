/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_diag.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
//#include <dk_buttons_and_leds.h>
#endif

LOG_MODULE_REGISTER(diag, CONFIG_SLM_LOG_LEVEL);

#define DIAG_INTER_EVENT_PERIOD 3000
/* Request diagnostic update no more often than 1 minute (time in milliseconds). */
#define DIAG_UPDATE_PERIOD (60 * 1000)

static uint32_t slm_diag_event_mask;
static struct k_delayed_work slm_diag_update_work;

static void diag_event_update(struct k_work *work)
{
	static uint8_t current_diag_event, current_step;
	static bool led_on;
	static int64_t last_request_timestamp;

	if (slm_diag_event_mask == 0){
		ui_led_set_state(LED_ID_DIAG, UI_DIAG_OFF);
		k_delayed_work_submit(&slm_diag_update_work, K_MSEC(500));
		return;
	}

	LOG_DBG("Diag mask: %d event:%d", slm_diag_event_mask, current_diag_event);
	if (slm_diag_event_mask & 1 << current_diag_event) {
		led_on = !led_on;
		if (!led_on) {
			ui_led_set_state(LED_ID_DIAG, UI_DIAG_OFF);
			current_step++;
		} else {
			ui_led_set_state(LED_ID_DIAG, UI_DIAG_ON);
		}
		if(current_step == current_diag_event + 1) {
			last_request_timestamp = k_uptime_get();
			k_delayed_work_submit(&slm_diag_update_work,
					      K_MSEC(DIAG_INTER_EVENT_PERIOD));
			current_diag_event++;
			current_step = 0;
		} else {
			k_delayed_work_submit(&slm_diag_update_work,
					      K_MSEC(500));
		}
	} else if (current_diag_event < SLM_DIAG_EVENT_COUNT) {
		current_diag_event++;
		k_delayed_work_submit(&slm_diag_update_work, K_NO_WAIT);
	} else {
		if ((last_request_timestamp != 0) &&
		(k_uptime_get() - last_request_timestamp) < DIAG_UPDATE_PERIOD) {
			LOG_DBG("Diag led is updated less than 1 min ago");
			k_delayed_work_submit(&slm_diag_update_work,
					      K_MSEC(1000));
		} else {
			current_diag_event = SLM_DIAG_RADIO_FAIL;
			k_delayed_work_submit(&slm_diag_update_work, K_NO_WAIT);
		}
	}
}

int slm_diag_init(void)
{
	int err = 0;

	k_delayed_work_init(&slm_diag_update_work, diag_event_update);
	k_delayed_work_submit(&slm_diag_update_work, K_NO_WAIT);

	return err;
}

int slm_diag_uninit(void)
{
	int err = 0;

	k_delayed_work_cancel(&slm_diag_update_work);

	return err;
}

void slm_diag_set_event(enum slm_diag_event event)
{
	LOG_DBG("set diag event: %d", event);
	slm_diag_event_mask |= 1 << event;
}

void slm_diag_clear_event(enum slm_diag_event event)
{
	LOG_DBG("clr diag event: %d", event);
	slm_diag_event_mask &= ~(1UL << event);
}
