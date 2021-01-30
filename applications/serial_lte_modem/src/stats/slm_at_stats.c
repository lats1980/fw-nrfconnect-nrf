/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <net/socket.h>
#include "slm_util.h"
#include "slm_at_stats.h"

LOG_MODULE_REGISTER(stats, CONFIG_SLM_LOG_LEVEL);

#define THREAD_STACK_SIZE       KB(1)
#define THREAD_PRIORITY         K_LOWEST_APPLICATION_THREAD_PRIO
static K_THREAD_STACK_DEFINE(stats_thread_stack, THREAD_STACK_SIZE);

/**@brief List of supported AT commands. */
enum slm_stats_at_cmd_type {
	AT_STATS,
	AT_STATS_MAX
};

/**@brief STATS operations. */
enum slm_stats_operation {
	AT_STATS_STOP,
	AT_STATS_START
};

/** forward declaration of cmd handlers **/
static int handle_AT_STATS(enum at_cmd_type cmd_type);

/**@brief SLM AT Command list type. */
static slm_at_cmd_list_t stats_at_list[AT_STATS_MAX] = {
	{AT_STATS, "AT#XSTATS", handle_AT_STATS},
};

static struct slm_stats_ctx {
	int fd;				/* STATS socket */
} stats;

/* global functions defined in different resources */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_SLM_SOCKET_RX_MAX * 2];

static int do_stats_start(void)
{
	/* Open socket if it is not opened yet. */
	if (stats.fd == INVALID_SOCKET) {
		stats.fd = socket(AF_LTE, SOCK_DGRAM, NPROTO_AT);

		if (stats.fd == INVALID_SOCKET) {
			return -errno;
		}
	} else {
		LOG_ERR("Stats socket was already opened.");
		return -EINVAL;
	}

	return stats.fd;
}

static int do_stats_stop(void)
{
	/* Open socket if it is not opened yet. */
	if (stats.fd != INVALID_SOCKET) {
		if (close(stats.fd) != 0) {
			return -errno;
		}
	} else {
		LOG_ERR("Stats socket was not opened.");
		return -EINVAL;
	}

	return 0;
}

/**@brief handle AT#XSTATS commands
 *  AT#XSTATS=<op>
 *  AT#XSTATS?
 *  AT#XSTATS=?
 */
static int handle_AT_STATS(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;

	uint16_t op;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) == 0) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err < 0) {
			LOG_ERR("Fail to get op: %d", err);
			return err;
		}

		if (op == AT_STATS_START) {
			if (stats.fd != INVALID_SOCKET) {
				return -EINPROGRESS;
			}

			err = do_stats_start();
			if (err) {
				LOG_ERR("Fail to stop SLM stats. Error: %d", err);
			}
		} else if (op == AT_STATS_STOP) {
			err = do_stats_stop();
			if (err) {
				LOG_ERR("Fail to stop SLM stats. Error: %d", err);
			}
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		break;

	default:
		break;
	}

	return err;
}

static void stats_thread_fn(void *arg1, void *arg2, void *arg3)
{
	//int err;

	while (1) {
	#if 0
		/* Don't go any further until sending HTTP request */
		k_sem_take(&http_req_sem, K_FOREVER);
		err = do_http_request();
		if (err < 0) {
			LOG_ERR("do_http_request fail:%d", err);
			/* Disconnect from server */
			err = do_http_disconnect();
			if (err) {
				LOG_ERR("Fail to disconnect. Error: %d", err);
			}
		}
	#endif
	}
}

int slm_at_stats_init(void)
{
	return 0;
}

int slm_at_stats_uninit(void)
{
	return 0;
}

/**@brief API to handle STATS AT commands
 */
int slm_at_stats_parse(const char *at_cmd)
{
	int ret = -ENOENT;
	enum at_cmd_type type;

	for (int i = 0; i < AT_STATS_MAX; i++) {
		if (slm_util_cmd_casecmp(at_cmd, stats_at_list[i].string)) {
			ret = at_parser_params_from_str(at_cmd, NULL,
						&at_param_list);
			if (ret) {
				LOG_ERR("Failed to parse AT command %d", ret);
				return -EINVAL;
			}
			type = at_parser_cmd_type_get(at_cmd);
			ret = stats_at_list[i].handler(type);
			break;
		}
	}

	return ret;
}

/**@brief API to list STATS AT commands
 */
void slm_at_stats_clac(void)
{
	for (int i = 0; i < AT_STATS_MAX; i++) {
		sprintf(rsp_buf, "%s\r\n", stats_at_list[i].string);
		rsp_send(rsp_buf, strlen(rsp_buf));
	}
}

K_THREAD_DEFINE(stats_thread, K_THREAD_STACK_SIZEOF(stats_thread_stack),
		stats_thread_fn, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, 0);
