/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include "slm_util.h"
#include "slm_at_uc.h"
#include <settings/settings.h>

LOG_MODULE_REGISTER(slm_uc, CONFIG_SLM_LOG_LEVEL);

/* Length of user config subtree path (slm/uc/) */
#define SLM_UC_DIR_PATH_LEN	7
/* Length of key string */
#define SLM_UC_KEY_LEN		(SETTINGS_MAX_NAME_LEN - SLM_UC_DIR_PATH_LEN)
/* Length of value string */
#define SLM_UC_VALUE_LEN	SETTINGS_MAX_VAL_LEN

/* array to store user config key string in ascii format */
static uint8_t slm_uc_key[SLM_UC_KEY_LEN + 1];
/* array to store user config value string in hex format */
static uint8_t slm_uc_value[SETTINGS_MAX_VAL_LEN];

/* global functions defined in different resources */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];

/**@brief User Config operations. */
enum slm_uc_operations {
	SLM_UC_OP_WRITE,
	SLM_UC_OP_LIST,
	SLM_UC_OP_READ,
	SLM_UC_OP_DELETE
};

static int do_uc_write(const uint8_t *uc_key, const uint8_t *uc_value, uint16_t uc_value_len)
{
	int ret = 0;

	LOG_DBG("User Config Write - key: %s", log_strdup(uc_key));
	LOG_HEXDUMP_DBG(uc_value, uc_value_len, "User Config Write - value:");

	sprintf(rsp_buf, "slm/uc/%s", uc_key);
	ret = settings_save_one(rsp_buf,
		(void *)uc_value, (size_t)uc_value_len);
	if (ret) {
		LOG_ERR("Fail to save key value");
		return ret;
	}

	return ret;
}

static int settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
			  void *cb_arg, void *param)
{
	int ret = 0;

	LOG_DBG("User config loaded. key: %s value len: %d", log_strdup(name), len);
	if (param) {
		size_t read_name_len = strlen((const char *)param);
		size_t name_len = strlen(name);

		if ((read_name_len != name_len) || (strncmp(name, param, read_name_len) != 0)) {
			LOG_DBG("key/value pair ignored");
			return 0;
		}
	}
	memset(slm_uc_value, 0, sizeof(slm_uc_value));
	ret = read_cb(cb_arg, slm_uc_value, len);
	if (ret > 0) {
		LOG_DBG("%d byte read", ret);
	} else {
		LOG_ERR("Fail to read value: %d", ret);
		return -EINVAL;
	}
	memset(rsp_buf, 0, sizeof(rsp_buf));
	sprintf(rsp_buf, "\r\n#XUC: \"%s\",\"", name);
	rsp_send(rsp_buf, strlen(rsp_buf));
	ret = slm_util_htoa(slm_uc_value, len, rsp_buf, len*2);
	if (ret <= 0) {
		LOG_ERR("hex convert error: %d", ret);
		return -EINVAL;
	}
	rsp_send(rsp_buf, ret);
	rsp_send("\"\r\n", 3);

	return 0;
}

static int do_uc_list(void)
{
	int ret = 0;

	ret = settings_load_subtree_direct("slm/uc", settings_load_cb, NULL);
	if (ret) {
		LOG_ERR("Fail to load user configuration");
	}

	return ret;
}

static int do_uc_read(const uint8_t *uc_key)
{
	int ret = 0;

	ret = settings_load_subtree_direct("slm/uc", settings_load_cb, (void *)uc_key);
	if (ret) {
		LOG_ERR("Fail to load user configuration");
	}

	return ret;
}

static int do_uc_delete(const uint8_t *uc_key)
{
	int err = 0;

	sprintf(rsp_buf, "slm/uc/%s", uc_key);
	err = settings_delete(rsp_buf);
	if (err) {
		LOG_ERR("Fail to delete %s. Err: %d", log_strdup(rsp_buf), err);
	} else {
		LOG_INF("%s deleted", log_strdup(rsp_buf));
	}

	return err;
}

/**@brief handle AT#XUC commands
 *  AT#XUC=<op>[,<key>[,<value>]]
 *  AT#XUC? READ command not supported
 *  AT#XUC=?
 */
int handle_at_uc(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op = 0;
	size_t ascii_len = 0;

	memset(rsp_buf, 0, sizeof(rsp_buf));
	memset(slm_uc_key, 0, sizeof(slm_uc_key));
	memset(slm_uc_value, 0, sizeof(slm_uc_value));

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) == 0) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err < 0) {
			LOG_ERR("Fail to get pin: %d", err);
			return err;
		}
		if (op > SLM_UC_OP_DELETE) {
			LOG_ERR("Fail to operate gpio: %d", op);
			return -EINVAL;
		}
		if (op == SLM_UC_OP_WRITE) {
			ascii_len = SLM_UC_KEY_LEN + 1;
			err = util_string_get(&at_param_list, 2, slm_uc_key, &ascii_len);
			if (err < 0) {
				LOG_ERR("Fail to get key string");
				return err;
			}
			ascii_len = SLM_UC_VALUE_LEN * 2 + 1;
			err = util_string_get(&at_param_list, 3, rsp_buf, &ascii_len);
			if (err < 0) {
				LOG_ERR("Fail to get value string");
				return err;
			}
			err = slm_util_atoh(rsp_buf, ascii_len, slm_uc_value, ascii_len/2);
			if (err < 0) {
				LOG_ERR("Fail to decode hex string to hex array");
				return err;
			}
			err = do_uc_write(slm_uc_key, slm_uc_value, err);
			if (err < 0) {
				return err;
			}
		} else if (op == SLM_UC_OP_LIST) {
			err = do_uc_list();
			if (err < 0) {
				return err;
			}
		} else if (op == SLM_UC_OP_READ) {
			ascii_len = SLM_UC_KEY_LEN + 1;
			err = util_string_get(&at_param_list, 2, slm_uc_key, &ascii_len);
			if (err < 0) {
				LOG_ERR("Fail to get key string");
				return err;
			}
			err = do_uc_read(slm_uc_key);
			if (err < 0) {
				return err;
			}
		} else if (op == SLM_UC_OP_DELETE) {
			ascii_len = SLM_UC_KEY_LEN + 1;
			err = util_string_get(&at_param_list, 2, slm_uc_key, &ascii_len);
			if (err < 0) {
				LOG_ERR("Fail to get key string");
				return err;
			}
			err = do_uc_delete(slm_uc_key);
			if (err < 0) {
				return err;
			}
		}
		break;
	case AT_CMD_TYPE_READ_COMMAND:
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		break;

	default:
		break;
	}

	return err;
}

int slm_at_uc_init(void)
{
	int err = 0;

	return err;
}

int slm_at_uc_uninit(void)
{
	int err = 0;

	return err;
}
