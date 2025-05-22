/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>

#include "ot_coap_utils.h"

LOG_MODULE_REGISTER(coap_server, CONFIG_COAP_SERVER_LOG_LEVEL);

#define OT_CONNECTION_LED DK_LED1
#define PROVISIONING_LED DK_LED3
#define OT_LEADER_LED DK_LED2
#define LIGHT_LED DK_LED4

#define COAP_SERVER_WORKQ_STACK_SIZE 512
#define COAP_SERVER_WORKQ_PRIORITY 5

K_THREAD_STACK_DEFINE(coap_server_workq_stack_area, COAP_SERVER_WORKQ_STACK_SIZE);
static struct k_work_q coap_server_workq;

static struct k_work provisioning_work;
static struct k_work traceroute_source_work;

#define TRACEROUTE_RESULT_TIMEOUT K_SECONDS(10)
static struct k_work_delayable traceroute_result_timeout_work;

static struct k_timer led_timer;
static struct k_timer provisioning_timer;

static bool is_connected;
static bool is_waiting_traceroute_result;

static void on_light_request(uint8_t command)
{
	static uint8_t val;

	switch (command) {
	case THREAD_COAP_UTILS_LIGHT_CMD_ON:
		dk_set_led_on(LIGHT_LED);
		val = 1;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_OFF:
		dk_set_led_off(LIGHT_LED);
		val = 0;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE:
		val = !val;
		dk_set_led(LIGHT_LED, val);
		break;

	default:
		break;
	}
}

static otCoapCode on_traceroute_request(uint16_t src_rloc16, uint16_t dst_rloc16,
				      uint8_t hops, uint8_t *path)
{
	int ret;
	uint16_t local_rloc16 = otThreadGetRloc16(openthread_get_default_context()->instance);

	// Check if dst_rloc16 is same as local RLOC16
	if (dst_rloc16 == local_rloc16) {
		LOG_INF("Destination RLOC16 is same as local RLOC16: 0x%04x", local_rloc16);
		for (uint8_t i = 0; i < hops; i++) {
			uint16_t rloc16 = (uint16_t)path[i] << 10;
			LOG_INF("Hop %d Router ID: %d, RLOC16: 0x%04x", i + 1, path[i], rloc16);
		}
#ifdef CONFIG_OPENTHREAD_FTD
		LOG_INF("Total hops %d", hops);
#else
		LOG_INF("Hop %d Child ID: %d, RLOC16: 0x%04x", hops + 1, local_rloc16 & 0xFF, local_rloc16);
		LOG_INF("Total hops %d", hops + 1);
#endif
		is_waiting_traceroute_result = false;
		k_work_cancel_delayable(&traceroute_result_timeout_work);
	} else {
		ret = traceroute(src_rloc16, dst_rloc16, hops, path);
		if (ret < 0) {
			LOG_ERR("Traceroute failed, error: %d", ret);
			return OT_COAP_CODE_SERVICE_UNAVAILABLE;
		}
	}
	dk_set_led_on(LIGHT_LED);
	return OT_COAP_CODE_CHANGED;
}

static void activate_provisioning(struct k_work *item)
{
	ARG_UNUSED(item);

	ot_coap_activate_provisioning();

	k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));
	k_timer_start(&provisioning_timer, K_SECONDS(5), K_NO_WAIT);

	LOG_INF("Provisioning activated");
}

static void activate_traceroute_source(struct k_work *item)
{
	ARG_UNUSED(item);

	ot_coap_activate_traceroute_source();

	LOG_INF("Traceroute source activated");
}

static void traceroute_result_timeout_handler(struct k_work *work)
{
	LOG_INF("Traceroute result timeout expired");
	is_waiting_traceroute_result = false;
	k_work_cancel_delayable(&traceroute_result_timeout_work);
}

static void deactivate_provisionig(void)
{
	k_timer_stop(&led_timer);
	k_timer_stop(&provisioning_timer);

	if (ot_coap_is_provisioning_active()) {
		ot_coap_deactivate_provisioning();
		LOG_INF("Provisioning deactivated");
	}
}

static void on_provisioning_timer_expiry(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);

	deactivate_provisionig();
}

static void on_led_timer_expiry(struct k_timer *timer_id)
{
	static uint8_t val = 1;

	ARG_UNUSED(timer_id);

	dk_set_led(PROVISIONING_LED, val);
	val = !val;
}

static void on_led_timer_stop(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);

	dk_set_led_off(PROVISIONING_LED);
}

static void on_button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t buttons = button_state & has_changed;

	if (!is_connected) {
		LOG_INF("Not connected, ignoring button press");
		return;
	}

	if (buttons & DK_BTN1_MSK) {
		uint8_t leader_router_id;
		uint16_t leader_rloc16;
		uint16_t local_rloc16;

		leader_router_id = otThreadGetLeaderRouterId(openthread_get_default_context()->instance);
		leader_rloc16 = (leader_router_id << 10);
		local_rloc16 = otThreadGetRloc16(openthread_get_default_context()->instance);
		if (traceroute(local_rloc16, leader_rloc16, 0, NULL) < 0) {
			LOG_ERR("Traceroute failed");
			return;
		} else {
			LOG_INF("Traceroute started to leader RLOC16: 0x%04x", leader_rloc16);
			dk_set_led_on(LIGHT_LED);
		}
	}

	if (buttons & DK_BTN2_MSK) {
		if (is_connected) {
			k_work_submit_to_queue(&coap_server_workq, &traceroute_source_work);
		} else {
			LOG_INF("Connection is broken");
		}
	}

	if (buttons & DK_BTN3_MSK) {
		uint16_t local_rloc16;

		if (is_waiting_traceroute_result) {
			LOG_WRN("Traceroute result is already in progress");
			return;
		}
		is_waiting_traceroute_result = true;
		local_rloc16 = otThreadGetRloc16(openthread_get_default_context()->instance);
		if (traceroute(TRACEROUTE_INIT_ADDR, local_rloc16, 0, NULL) < 0) {
			LOG_ERR("Traceroute request failed");
			return;
		} else {
			LOG_INF("Traceroute request started for RLOC16: 0x%04x", local_rloc16);
		}
		k_work_schedule(&traceroute_result_timeout_work, TRACEROUTE_RESULT_TIMEOUT);
	}

	if (buttons & DK_BTN4_MSK) {
		k_work_submit_to_queue(&coap_server_workq, &provisioning_work);
	}
}

static void on_thread_state_changed(otChangedFlags flags, struct openthread_context *ot_context,
				    void *user_data)
{
	if (flags & OT_CHANGED_THREAD_ROLE) {
		switch (otThreadGetDeviceRole(ot_context->instance)) {
		case OT_DEVICE_ROLE_LEADER:
			dk_set_led_on(OT_CONNECTION_LED);
			dk_set_led_on(OT_LEADER_LED);
			is_connected = true;
			break;
		case OT_DEVICE_ROLE_CHILD:
		case OT_DEVICE_ROLE_ROUTER:
			dk_set_led_on(OT_CONNECTION_LED);
			dk_set_led_off(OT_LEADER_LED);
			is_connected = true;
			break;
		case OT_DEVICE_ROLE_DISABLED:
		case OT_DEVICE_ROLE_DETACHED:
		default:
			dk_set_led_off(OT_CONNECTION_LED);
			dk_set_led_off(OT_LEADER_LED);
			is_connected = false;
			deactivate_provisionig();
			break;
		}
	}
}

static struct openthread_state_changed_cb ot_state_chaged_cb = { .state_changed_cb =
									 on_thread_state_changed };

int main(void)
{
	int ret;

	LOG_INF("Start CoAP-server sample");

	k_timer_init(&led_timer, on_led_timer_expiry, on_led_timer_stop);
	k_timer_init(&provisioning_timer, on_provisioning_timer_expiry, NULL);

	k_work_queue_init(&coap_server_workq);
	k_work_queue_start(&coap_server_workq, coap_server_workq_stack_area,
					K_THREAD_STACK_SIZEOF(coap_server_workq_stack_area),
					COAP_SERVER_WORKQ_PRIORITY, NULL);
	k_work_init(&provisioning_work, activate_provisioning);
	k_work_init(&traceroute_source_work, activate_traceroute_source);
	k_work_init_delayable(&traceroute_result_timeout_work, traceroute_result_timeout_handler);

	ret = ot_coap_init(&deactivate_provisionig, &on_light_request, &on_traceroute_request);
	if (ret) {
		LOG_ERR("Could not initialize OpenThread CoAP");
		goto end;
	}

	ret = dk_leds_init();
	if (ret) {
		LOG_ERR("Could not initialize leds, err code: %d", ret);
		goto end;
	}

	ret = dk_buttons_init(on_button_changed);
	if (ret) {
		LOG_ERR("Cannot init buttons (error: %d)", ret);
		goto end;
	}

	openthread_state_changed_cb_register(openthread_get_default_context(), &ot_state_chaged_cb);
	openthread_start(openthread_get_default_context());

end:
	return 0;
}
