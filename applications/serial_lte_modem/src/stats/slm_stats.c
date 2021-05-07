/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <net/socket.h>
#include <modem/lte_lc.h>
#include <modem/at_cmd_parser.h>
#include "slm_util.h"
#include "slm_stats.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#include "slm_diag.h"
#include <dk_buttons_and_leds.h>
#endif

LOG_MODULE_REGISTER(stats, CONFIG_SLM_LOG_LEVEL);

static struct k_thread stats_thread;
#define STATS_THREAD_STACK_SIZE       KB(1)
#define STATS_THREAD_PRIORITY	 K_LOWEST_APPLICATION_THREAD_PRIO
static K_THREAD_STACK_DEFINE(stats_thread_stack, STATS_THREAD_STACK_SIZE);

#define SLM_STATS_MAX_READ_LENGTH		128

#define AT_CMD_CEREG_5				"AT+CEREG=5"
#define AT_CEREG_READ				"AT+CEREG?"
#define AT_CMD_CESQ_ON				"AT%CESQ=1"
#define AT_CMD_CESQ_RESP			"%CESQ"
#define AT_CMD_VBAT				"AT%XVBAT"
#define AT_CMD_VBATLOWLVL_READ			"AT%XVBATLOWLVL?"
#define AT_CEREG_PARAMS_COUNT_MAX		10
#define AT_CEREG_REG_STATUS_INDEX		1
#define AT_CEREG_TAC_INDEX			2
#define AT_CEREG_CELL_ID_INDEX			3
#define AT_CEREG_ACTIVE_TIME_INDEX		7
#define AT_CEREG_READ_ACTIVE_TIME_INDEX		8
#define AT_CEREG_TAU_INDEX			8
#define AT_CEREG_READ_TAU_INDEX			9

#define GET_STATS_BUF_LEN			100
#define SUBSCRIBE_STATS_BUF_LEN			10

static struct k_delayed_work batlvl_read;

static struct slm_stats_ctx {
	int fd;
	enum lte_lc_nw_reg_status reg_status;
	uint16_t rsrp;
	uint16_t xvbat;
} stats;

/* File descriptor */
static struct pollfd fds = {
	.fd = INVALID_SOCKET,
};

enum lte_lc_notif_type {
	LTE_LC_NOTIF_CEREG,
	LTE_LC_NOTIF_CSCON,
	LTE_LC_NOTIF_CEDRXP,
	LTE_LC_NOTIF_CESQ,
	LTE_LC_NOTIF_XVBATLOWLVL,
	LTE_LC_NOTIF_XVBAT,

	LTE_LC_NOTIF_COUNT,
};

static const char *const at_notifs[] = {
	[LTE_LC_NOTIF_CEREG] = "+CEREG",
	[LTE_LC_NOTIF_CSCON] = "+CSCON",
	[LTE_LC_NOTIF_CEDRXP] = "+CEDRXP",
	[LTE_LC_NOTIF_CESQ] = "%CESQ",
	[LTE_LC_NOTIF_XVBATLOWLVL] = "%XVBATLOWLVL",
	[LTE_LC_NOTIF_XVBAT] = "%XVBAT",
};

BUILD_ASSERT(ARRAY_SIZE(at_notifs) == LTE_LC_NOTIF_COUNT);

static bool is_relevant_notif(const char *notif, enum lte_lc_notif_type *type)
{
	for (size_t i = 0; i < ARRAY_SIZE(at_notifs); i++) {
		if (strncmp(at_notifs[i], notif,
			    strlen(at_notifs[i])) == 0) {
			/* The notification type matches the array index */
			*type = i;

			return true;
		}
	}

	return false;
}

static int parse_psm_cfg(struct at_param_list *at_params,
			 bool is_notif,
			 struct lte_lc_psm_cfg *psm_cfg)
{
	int err;
	size_t tau_idx = is_notif ? AT_CEREG_TAU_INDEX :
				    AT_CEREG_READ_TAU_INDEX;
	size_t active_time_idx = is_notif ? AT_CEREG_ACTIVE_TIME_INDEX :
					    AT_CEREG_READ_ACTIVE_TIME_INDEX;
	char timer_str[9] = {0};
	char unit_str[4] = {0};
	size_t timer_str_len = sizeof(timer_str) - 1;
	size_t unit_str_len = sizeof(unit_str) - 1;
	size_t lut_idx;
	uint32_t timer_unit, timer_value;

	/* Lookup table for T3324 timer used for PSM active time in seconds.
	 * Ref: GPRS Timer 2 IE in 3GPP TS 24.008 Table 10.5.163/3GPP TS 24.008.
	 */
	static const uint32_t t3324_lookup[8] = {2, 60, 600, 60, 60, 60, 60, 0};

	/* Lookup table for T3412 timer used for periodic TAU. Unit is seconds.
	 * Ref: GPRS Timer 3 in 3GPP TS 24.008 Table 10.5.163a/3GPP TS 24.008.
	 */
	static const uint32_t t3412_lookup[8] = {600, 3600, 36000, 2, 30, 60,
					      1152000, 0};

	/* Parse periodic TAU string */
	err = at_params_string_get(at_params,
				   tau_idx,
				   timer_str,
				   &timer_str_len);
	if (err) {
		LOG_ERR("Could not get TAU, error: %d", err);
		return err;
	}

	memcpy(unit_str, timer_str, unit_str_len);

	lut_idx = strtoul(unit_str, NULL, 2);
	if (lut_idx > (ARRAY_SIZE(t3412_lookup) - 1)) {
		LOG_ERR("Unable to parse periodic TAU string");
		err = -EINVAL;
		return err;
	}

	timer_unit = t3412_lookup[lut_idx];
	timer_value = strtoul(timer_str + unit_str_len, NULL, 2);
	psm_cfg->tau = timer_unit ? timer_unit * timer_value : -1;

	/* Parse active time string */
	err = at_params_string_get(at_params,
				   active_time_idx,
				   timer_str,
				   &timer_str_len);
	if (err) {
		LOG_ERR("Could not get TAU, error: %d", err);
		return err;
	}

	memcpy(unit_str, timer_str, unit_str_len);

	lut_idx = strtoul(unit_str, NULL, 2);
	if (lut_idx > (ARRAY_SIZE(t3324_lookup) - 1)) {
		LOG_ERR("Unable to parse active time string");
		err = -EINVAL;
		return err;
	}

	timer_unit = t3324_lookup[lut_idx];
	timer_value = strtoul(timer_str + unit_str_len, NULL, 2);
	psm_cfg->active_time = timer_unit ? timer_unit * timer_value : -1;

	LOG_DBG("TAU: %d sec, active time: %d sec\n",
		psm_cfg->tau, psm_cfg->active_time);

	return 0;
}

static int parse_cereg(const char *notification,
		       enum lte_lc_nw_reg_status *reg_status,
		       struct lte_lc_cell *cell,
		       struct lte_lc_psm_cfg *psm_cfg)
{
	int err, status;
	struct at_param_list resp_list;
	char str_buf[10];
	size_t len = sizeof(str_buf) - 1;

	err = at_params_list_init(&resp_list, AT_CEREG_PARAMS_COUNT_MAX);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);
		return err;
	}

	/* Parse CEREG response and populate AT parameter list */
	err = at_parser_params_from_str(notification,
					NULL,
					&resp_list);
	if (err) {
		LOG_ERR("Could not parse AT+CEREG response, error: %d", err);
		goto clean_exit;
	}

	/* Parse network registration status */
	err = at_params_int_get(&resp_list,
				AT_CEREG_REG_STATUS_INDEX,
				&status);
	if (err) {
		LOG_ERR("Could not get registration status, error: %d", err);
		goto clean_exit;
	}

	*reg_status = status;

	if ((*reg_status != LTE_LC_NW_REG_UICC_FAIL) &&
	    (at_params_valid_count_get(&resp_list) > AT_CEREG_CELL_ID_INDEX)) {
		/* Parse tracking area code */
		err = at_params_string_get(&resp_list,
					AT_CEREG_TAC_INDEX,
					str_buf, &len);
		if (err) {
			LOG_ERR("Could not get tracking area code,"
				" error: %d", err);
			goto clean_exit;
		}

		str_buf[len] = '\0';
		cell->tac = strtoul(str_buf, NULL, 16);

		/* Parse cell ID */
		len = sizeof(str_buf) - 1;

		err = at_params_string_get(&resp_list,
					AT_CEREG_CELL_ID_INDEX,
					str_buf, &len);
		if (err) {
			LOG_ERR("Could not get cell ID, error: %d", err);
			goto clean_exit;
		}

		str_buf[len] = '\0';
		cell->id = strtoul(str_buf, NULL, 16);
	} else {
		cell->tac = UINT32_MAX;
		cell->id = UINT32_MAX;
	}

	/* Parse PSM configuration only when registered */
	if (((*reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
	    (*reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) &&
	     (at_params_valid_count_get(&resp_list) > AT_CEREG_TAU_INDEX)) {
		err = parse_psm_cfg(&resp_list, true, psm_cfg);
		if (err) {
			LOG_ERR("Failed to parse PSM configuration, error: %d",
				err);
			goto clean_exit;
		}
	} else {
		/* When device is not registered, PSM valies are invalid */
		psm_cfg->tau = -1;
		psm_cfg->active_time = -1;
	}

clean_exit:
	at_params_list_free(&resp_list);

	return err;
}

int get_stats(void)
{
	int err = 0;
	char buf[GET_STATS_BUF_LEN];

	/* Read network registration status */
	err = at_cmd_write(AT_CEREG_READ, buf, sizeof(buf), NULL);
	if (err) {
		LOG_ERR("Could not get CEREG response, error: %d", err);
		return err;
	}
	stats.reg_status = atoi(buf + strlen("+CEREG: 0,"));
	if (stats.reg_status == LTE_LC_NW_REG_UICC_FAIL) {
		LOG_ERR("Network registration fail: UICC");
#if defined(CONFIG_SLM_DIAG)
		slm_diag_set_event(SLM_DIAG_UICC_FAIL);
#endif
	} else if (stats.reg_status == LTE_LC_NW_REG_SEARCHING) {
		LOG_DBG("Network registration status: Connecting");
		ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTING);
	} else if ((stats.reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		(stats.reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
		LOG_DBG("Network registration status: %s",
		stats.reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
		"Connected - home network" : "Connected - roaming");
		ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTED);
#if defined(CONFIG_SLM_DIAG)
		slm_diag_clear_event(SLM_DIAG_UICC_FAIL);
#endif
	} else if ((stats.reg_status == LTE_LC_NW_REG_NOT_REGISTERED) ||
		(stats.reg_status == LTE_LC_NW_REG_UNKNOWN)) {
		ui_led_set_state(LED_ID_LTE, UI_LTE_DISCONNECTED);
	}

	return err;
}

static int subscribe_stats(void)
{
	int bytes_sent, bytes_recv;
	char buf[SUBSCRIBE_STATS_BUF_LEN];

	if (stats.fd != INVALID_SOCKET) {
		bytes_sent = send(stats.fd, AT_CMD_CEREG_5,
				  strlen(AT_CMD_CEREG_5), 0);
		if (bytes_sent != strlen(AT_CMD_CEREG_5)) {
			return -1;
		}
		bytes_recv = recv(stats.fd, buf, 10, 0);
		if (strncmp("OK", buf, 2) != 0) {
			return -1;
		}
		bytes_sent = send(stats.fd, AT_CMD_CESQ_ON,
				  strlen(AT_CMD_CESQ_ON), 0);
		if (bytes_sent != strlen(AT_CMD_CESQ_ON)) {
			return -1;
		}
		bytes_recv = recv(stats.fd, buf, 10, 0);
		if (strncmp("OK", buf, 2) != 0) {
			return -1;
		}
	}

	return 0;
}

static void stats_thread_fn(void *arg1, void *arg2, void *arg3)
{
	int err;
	int bytes_read;
	static char buf[SLM_STATS_MAX_READ_LENGTH];
	enum lte_lc_notif_type notif_type;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (stats.fd != INVALID_SOCKET) {
		err = poll(&fds, 1, -1);
		if (err < 0) {
			LOG_ERR("ERROR: poll %d", errno);
			break;
		}
		if ((fds.revents & POLLIN) == POLLIN) {
			bytes_read = recv(stats.fd, buf, sizeof(buf), 0);

			/* Handle possible socket-level errors */
			if (bytes_read < 0) {
				LOG_ERR("Unrecoverable reception error (err: %d), "
					"thread killed", errno);
				if (stats.fd != INVALID_SOCKET) {
					close(stats.fd);
				}
				stats.fd = INVALID_SOCKET;
			} else if (bytes_read == 0) {
				LOG_ERR("AT message empty");
			} else if (buf[bytes_read - 1] != '\0') {
				LOG_ERR("AT message too large for reception buffer or "
					"missing termination character");
			}

			LOG_DBG("at_cmd_rx %d bytes, %s", bytes_read, log_strdup(buf));
			/* Only proceed with parsing if notification is relevant */
			if (!is_relevant_notif(buf, &notif_type)) {
				LOG_DBG("Notification without interests: %s",
					log_strdup(buf));
				continue;
			}

			switch (notif_type) {
			case LTE_LC_NOTIF_CEREG: {
				struct lte_lc_cell cell;
				struct lte_lc_psm_cfg psm_cfg;

				err = parse_cereg(buf, &stats.reg_status,
						&cell, &psm_cfg);
				if (err) {
					LOG_ERR("Failed to parse notification"
						" (error %d): %s",
						err, log_strdup(buf));
				}
				LOG_DBG("reg_status: %hu", stats.reg_status);

				if (stats.reg_status == LTE_LC_NW_REG_UICC_FAIL) {
					LOG_ERR("Network registration fail: UICC");
	#if defined(CONFIG_SLM_DIAG)
					slm_diag_set_event(SLM_DIAG_UICC_FAIL);
	#endif
				} else if (stats.reg_status == LTE_LC_NW_REG_SEARCHING) {
					LOG_DBG("Network registration status: Connecting");
					ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTING);
				} else if ((stats.reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
					(stats.reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
					LOG_DBG("Network registration status: %s",
					stats.reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
					"Connected - home network" : "Connected - roaming");
					ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTED);
	#if defined(CONFIG_SLM_DIAG)
					slm_diag_clear_event(SLM_DIAG_UICC_FAIL);
	#endif
				} else if ((stats.reg_status == LTE_LC_NW_REG_NOT_REGISTERED) ||
					(stats.reg_status == LTE_LC_NW_REG_UNKNOWN)) {
					ui_led_set_state(LED_ID_LTE, UI_LTE_DISCONNECTED);
				}
				break;
			}
			case LTE_LC_NOTIF_CESQ: {
				/* The format of %CESQ info:
				 * %CESQ: <rsrp>,...
				 */
				stats.rsrp = atoi(buf + strlen("%CESQ: "));

				LOG_DBG("rsrp: %hu", stats.rsrp);

				/* Only send a value from a valid range (0 - 97). */
				if (stats.rsrp > 97) {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_OFF);
					break;
				}

				if (stats.rsrp < RSRP_THRESHOLD_1) {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L0);
				} else if (stats.rsrp >= RSRP_THRESHOLD_1 &&
					   stats.rsrp < RSRP_THRESHOLD_2) {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L1);
				} else if (stats.rsrp >= RSRP_THRESHOLD_2 &&
					   stats.rsrp < RSRP_THRESHOLD_3) {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L2);
				} else if (stats.rsrp >= RSRP_THRESHOLD_3 &&
					   stats.rsrp < RSRP_THRESHOLD_4) {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L3);
				} else {
					ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L4);
				}
				break;
			}
			case LTE_LC_NOTIF_XVBATLOWLVL: {
				/* The format of %XVBATLOWLVL:
				 * %VBATLOWLVL: <lowlvl>,...
				 */
				int xvbatlowlvl = 0;

				xvbatlowlvl = atoi(buf + strlen("%XVBATLOWLVL: "));
				LOG_DBG("Current lvl: %hu Threshold: %hu", stats.xvbat, (uint16_t)xvbatlowlvl);
				if (stats.xvbat > (uint16_t)xvbatlowlvl) {
					stats.xvbat = 0;
					slm_diag_clear_event(SLM_DIAG_LOW_BATTERY);
				} else {
					LOG_DBG("Low battery. Check again after %d ms",
						CONFIG_SLM_STATS_BATTERY_INTERVAL);
#if defined(CONFIG_SLM_DIAG)
					slm_diag_set_event(SLM_DIAG_LOW_BATTERY);
#endif
				}
				break;
			}
			case LTE_LC_NOTIF_XVBAT: {
				/* The format of %XVBAT:
				 * %XVBAT: <lvl>,...
				 */
				stats.xvbat = atoi(buf + strlen("%XVBAT: "));
				LOG_DBG("battery level: %hu", stats.xvbat);
				break;
			}
			default:
				LOG_ERR("Unrecognized notification type: %d",
					notif_type);
				break;
			}
		}
		if ((fds.revents & POLLERR) == POLLERR) {
			LOG_ERR("POLLERR");
		}
		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			LOG_ERR("POLLNVAL");
		}
	}
	LOG_DBG("Exit STATS thread");
}

static int do_stats_start(void)
{
	int err = 0;

	/* Open socket if it is not opened yet. */
	if (stats.fd == INVALID_SOCKET) {
		stats.fd = socket(AF_LTE, SOCK_DGRAM, NPROTO_AT);

		if (stats.fd == INVALID_SOCKET) {
			LOG_ERR("Fail to open socket.");
			return -errno;
		}
		fds.fd = stats.fd;
		fds.events = POLLIN;
		err = get_stats();
		if (err != 0) {
			LOG_ERR("Fail to get current stats");
			return err;
		}
		err = subscribe_stats();
		if (err != 0) {
			LOG_ERR("Fail to subscribe stats");
			return err;
		}
		k_delayed_work_submit(&batlvl_read,
				K_MSEC(CONFIG_SLM_STATS_BATTERY_INTERVAL));
		/* start stats thread */
		k_thread_create(&stats_thread, stats_thread_stack,
				K_THREAD_STACK_SIZEOF(stats_thread_stack),
				stats_thread_fn, NULL, NULL, NULL,
				STATS_THREAD_PRIORITY, K_USER, K_NO_WAIT);
	} else {
		LOG_ERR("Stats socket was already opened.");
		return -EINVAL;
	}

	return err;
}

static int do_stats_stop(void)
{
	k_delayed_work_cancel(&batlvl_read);
	/* Close socket if it is opened. */
	if (stats.fd != INVALID_SOCKET) {
		if (close(stats.fd) != 0) {
			LOG_ERR("Fail to close socket.");
			return -errno;
		}
		stats.fd = INVALID_SOCKET;
		fds.fd = INVALID_SOCKET;
	} else {
		LOG_ERR("Stats socket was not opened.");
		return -EINVAL;
	}

	return 0;
}

int slm_stats_subscribe(void)
{
	int bytes_sent = 0;
	int err = 0;

	if (stats.fd == INVALID_SOCKET) {
		LOG_ERR("Not able to subscribe stats");
		return -EINVAL;
	}

	LOG_DBG("Subscribe stats");
	bytes_sent = send(stats.fd, AT_CMD_CEREG_5,
				strlen(AT_CMD_CEREG_5), 0);
	if (bytes_sent != strlen(AT_CMD_CEREG_5)) {
		err = -errno;
	}

	bytes_sent = send(stats.fd, AT_CMD_CESQ_ON,
				strlen(AT_CMD_CESQ_ON), 0);
	if (bytes_sent != strlen(AT_CMD_CESQ_ON)) {
		err = -errno;
	}

	return err;
}

static void batlvl_read_fn(struct k_work *work)
{
	int bytes_sent = 0;

	if (stats.fd == INVALID_SOCKET) {
		LOG_ERR("Not able to read vbat");
	}
	/* Read xvbat */
	bytes_sent = send(stats.fd, AT_CMD_VBAT,
				strlen(AT_CMD_VBAT), 0);
	if (bytes_sent != strlen(AT_CMD_VBAT)) {
		LOG_ERR("Fail to send AT_CMD_VBAT command: %d", -errno);
	}
	/* Read xvbatlowlvl */
	bytes_sent = send(stats.fd, AT_CMD_VBATLOWLVL_READ,
				strlen(AT_CMD_VBATLOWLVL_READ), 0);
	if (bytes_sent != strlen(AT_CMD_VBATLOWLVL_READ)) {
		LOG_ERR("Fail to send VBATLOWLVL_READ command: %d", -errno);
	}
	k_delayed_work_submit(&batlvl_read,
				K_MSEC(CONFIG_SLM_STATS_BATTERY_INTERVAL));
}

int slm_stats_get_nw_reg_status(void)
{
	LOG_DBG("NW reg status: %d", (int)stats.reg_status);
	return (int)stats.reg_status;
}

int slm_stats_init(void)
{
	int err;

	stats.fd = INVALID_SOCKET;
	stats.xvbat = 0;
	k_delayed_work_init(&batlvl_read, batlvl_read_fn);
	err = do_stats_start();
	if (err) {
		LOG_ERR("Fail to start SLM stats. Error: %d", err);
	}

	return 0;
}

int slm_stats_uninit(void)
{
	int err;

	err = do_stats_stop();
	if (err) {
		LOG_ERR("Fail to stop SLM stats. Error: %d", err);
	}

	return 0;
}
