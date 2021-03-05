/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <drivers/i2c.h>
#include <stdio.h>
#include "slm_util.h"
#include "slm_at_twi.h"

LOG_MODULE_REGISTER(slm_twi, CONFIG_SLM_LOG_LEVEL);

#define TWI_MAX_INSTANCE	4
#define TWI_ADDR_LEN		2
#define TWI_DATA_LEN		255

/**@brief List of supported AT commands. */
enum slm_twi_at_cmd_type {
	AT_TWI_OPEN,
	AT_TWI_WRITE,
	AT_TWI_READ,
	AT_TWI_WRITE_READ,
	AT_TWI_MAX
};

/** forward declaration of cmd handlers **/
static int handle_at_twi_open(enum at_cmd_type cmd_type);
static int handle_at_twi_write(enum at_cmd_type cmd_type);
static int handle_at_twi_read(enum at_cmd_type cmd_type);
static int handle_at_twi_write_read(enum at_cmd_type cmd_type);

/**@brief SLM AT Command list type. */
static slm_at_cmd_list_t twi_at_list[AT_TWI_MAX] = {
	{AT_TWI_OPEN, "AT#XTWIOP", handle_at_twi_open},
	{AT_TWI_WRITE, "AT#XTWIW", handle_at_twi_write},
	{AT_TWI_READ, "AT#XTWIR", handle_at_twi_read},
	{AT_TWI_WRITE_READ, "AT#XTWIWR", handle_at_twi_write_read},
};

static const struct device *slm_twi_dev[TWI_MAX_INSTANCE];

/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_SLM_SOCKET_RX_MAX * 2];

static int do_twi_open(uint16_t idx)
{
	int err = -EINVAL;

	switch (idx) {
	case 0:
		slm_twi_dev[idx] = device_get_binding(DT_LABEL(DT_NODELABEL(i2c0)));
		break;
	case 1:
		slm_twi_dev[idx] = device_get_binding(DT_LABEL(DT_NODELABEL(i2c1)));
		break;
	case 2:
		slm_twi_dev[idx] = device_get_binding(DT_LABEL(DT_NODELABEL(i2c2)));
		break;
	case 3:
		slm_twi_dev[idx] = device_get_binding(DT_LABEL(DT_NODELABEL(i2c3)));
		break;
	default:
		return err;
	}

	if (!slm_twi_dev[idx]) {
		LOG_ERR("Could not get pointer to device");
		return err;
	}

	return 0;
}

static int do_twi_write(uint16_t index, uint16_t dev_addr,
			const uint8_t *twi_data_ascii, uint16_t ascii_len)
{
	int ret = -EINVAL;
	uint8_t twi_data_hex[ascii_len/2];

	if (!slm_twi_dev[index]) {
		LOG_ERR("TWI device is not opened");
		return ret;
	}

	ret = slm_util_atoh(twi_data_ascii, ascii_len, twi_data_hex, ascii_len/2);
	if (ret < 0) {
		LOG_ERR("Fail to decode hex string to hex array");
		return ret;
	}

	ret = i2c_write(slm_twi_dev[index], twi_data_hex, ret, dev_addr);
	if (ret < 0) {
		LOG_ERR("Fail to write twi data at address: %hx", dev_addr);
		return ret;
	}

	return ret;
}

static int do_twi_read(uint16_t index, uint16_t dev_addr, uint8_t num_read)
{
	int ret = -EINVAL;
	uint8_t twi_data[num_read];
	uint8_t twi_data_ascii[num_read*2];

	if (!slm_twi_dev[index]) {
		LOG_ERR("TWI device is not opened");
		return ret;
	}

	if (num_read > TWI_DATA_LEN) {
		LOG_ERR("Not enough buffer. Increase TWI_DATA_LEN");
		return -ENOBUFS;
	}

	ret = i2c_read(slm_twi_dev[index], twi_data, (uint32_t)num_read, dev_addr);
	if (ret < 0) {
		LOG_ERR("Fail to read twi data");
		return ret;
	}

	ret = slm_util_htoa(twi_data, num_read, twi_data_ascii, num_read*2);
	if (ret > 0) {
		sprintf(rsp_buf, "\r\n#XTWIR: %d\r\n", ret);
		rsp_send(rsp_buf, strlen(rsp_buf));
		rsp_send(twi_data_ascii, ret);
		ret = 0;
	} else {
		LOG_ERR("hex convert error: %d", ret);
		ret = -EINVAL;
	}

	return ret;
}

static int do_twi_write_read(uint16_t index, uint16_t dev_addr,
			     const uint8_t *twi_data_ascii_w, uint16_t ascii_len,
			     uint16_t num_read)
{
	int ret = -EINVAL;
	uint8_t twi_data_w[ascii_len/2];
	uint8_t twi_data_r[num_read], twi_data_ascii_r[num_read*2];

	if (!slm_twi_dev[index]) {
		LOG_ERR("TWI device is not opened");
		return ret;
	}

	/* Decode hex string to hex array */
	ret = slm_util_atoh(twi_data_ascii_w, ascii_len, twi_data_w, ascii_len/2);
	if (ret < 0) {
		LOG_ERR("Fail to decode hex string to hex array");
		return ret;
	}

	ret = i2c_write_read(slm_twi_dev[index], dev_addr,
			     twi_data_w, ret,
			     twi_data_r, num_read);
	if (ret < 0) {
		LOG_ERR("Fail to write and read data at address: %hx", dev_addr);
		return ret;
	}

	ret = slm_util_htoa(twi_data_r, num_read, twi_data_ascii_r, num_read*2);
	if (ret > 0) {
		sprintf(rsp_buf, "\r\n#XTWIWR: %d\r\n", ret);
		rsp_send(rsp_buf, strlen(rsp_buf));
		rsp_send(twi_data_ascii_r, ret);
		ret = 0;
	} else {
		LOG_ERR("hex convert error: %d", ret);
		ret = -EINVAL;
	}

	return ret;
}

/**@brief handle AT#XTWIOP commands
 *  AT#XTWIOP=<index>
 *  AT#XTWIOP? READ command not supported
 *  AT#XTWIOP=?
 */
static int handle_at_twi_open(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;

	uint16_t idx;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_short_get(&at_param_list, 1, &idx);
		if (err < 0) {
			LOG_ERR("Fail to get twi index: %d", err);
			return err;
		}
		err = do_twi_open(idx);
		break;
	case AT_CMD_TYPE_READ_COMMAND:
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTWIOP: (0,1,2,3)\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTWIW commands
 *  AT#XTWIW=<index>,<dev_addr>,<data>
 *  AT#XTWIW? READ command not supported
 *  AT#XTWIW=?
 */
static int handle_at_twi_write(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t index, dev_addr;
	uint8_t twi_addr_ascii[TWI_ADDR_LEN + 1];
	uint8_t twi_data_ascii[TWI_DATA_LEN + 1];
	size_t ascii_len;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) != 4) {
			LOG_ERR("Wrong input parameters");
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &index);
		if (err < 0) {
			LOG_ERR("Fail to get twi index: %d", err);
			return err;
		}
		ascii_len = TWI_ADDR_LEN + 1;
		err = util_string_get(&at_param_list, 2, twi_addr_ascii, &ascii_len);
		if (err < 0) {
			LOG_ERR("Fail to get device address");
			return err;
		}
		sscanf(twi_addr_ascii, "%hx", &dev_addr);
		LOG_DBG("dev_addr: %hx", dev_addr);
		ascii_len = TWI_DATA_LEN + 1;
		err = util_string_get(&at_param_list, 3, twi_data_ascii, &ascii_len);
		if (err) {
			return err;
		}
		LOG_DBG("Data to write: %s", log_strdup(twi_data_ascii));
		err = do_twi_write(index, dev_addr, twi_data_ascii, (uint16_t)ascii_len);
		if (err < 0) {
			return err;
		}
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTWIW: <index>,<dev_addr>,<data>\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTWIR commands
 *  AT#XTWIR=<index>,<dev_addr>,<num_read>
 *  AT#XTWIR? READ command not supported
 *  AT#XTWIR=?
 */
static int handle_at_twi_read(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t index, dev_addr, num_read;
	uint8_t twi_addr_ascii[TWI_ADDR_LEN + 1];
	size_t ascii_len;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) != 4) {
			LOG_ERR("Wrong input parameters");
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &index);
		if (err < 0) {
			LOG_ERR("Fail to get twi index: %d", err);
			return err;
		}
		ascii_len = TWI_ADDR_LEN + 1;
		err = util_string_get(&at_param_list, 2, twi_addr_ascii, &ascii_len);
		if (err < 0) {
			LOG_ERR("Fail to get device address: %d", err);
			return err;
		}
		sscanf(twi_addr_ascii, "%hx", &dev_addr);
		LOG_DBG("dev_addr: %hx", dev_addr);
		err = at_params_short_get(&at_param_list, 3, &num_read);
		if (err < 0) {
			LOG_ERR("Fail to get bytes to read: %d", err);
			return err;
		}
		if (num_read > TWI_DATA_LEN) {
			LOG_ERR("No enough buffer to read %d bytes", num_read);
			return -ENOBUFS;
		}
		err = do_twi_read(index, dev_addr, (uint8_t)num_read);
		if (err < 0) {
			return err;
		}
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTWIR: <index>,<dev_addr>,<num_read>\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTWIWR commands
 *  AT#XTWIWR=<index>,<dev_addr>,<data>,<num_read>
 *  AT#XTWIWR? READ command not supported
 *  AT#XTWIWR=?
 */
static int handle_at_twi_write_read(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t index, dev_addr, num_read;
	uint8_t twi_addr_ascii[TWI_ADDR_LEN + 1];
	uint8_t twi_data_ascii[TWI_DATA_LEN + 1];
	size_t ascii_len;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) != 5) {
			LOG_ERR("Wrong input parameters");
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &index);
		if (err < 0) {
			LOG_ERR("Fail to get twi index: %d", err);
			return err;
		}
		ascii_len = TWI_ADDR_LEN + 1;
		err = util_string_get(&at_param_list, 2, twi_addr_ascii, &ascii_len);
		if (err < 0) {
			LOG_ERR("Fail to get device address");
			return err;
		}
		sscanf(twi_addr_ascii, "%hx", &dev_addr);
		LOG_DBG("dev_addr: %hx", dev_addr);
		ascii_len = TWI_DATA_LEN + 1;
		err = util_string_get(&at_param_list, 3, twi_data_ascii, &ascii_len);
		if (err) {
			return err;
		}
		LOG_DBG("Data to write: %s", log_strdup(twi_data_ascii));
		err = at_params_short_get(&at_param_list, 4, &num_read);
		if (err < 0) {
			LOG_ERR("Fail to get twi index: %d", err);
			return err;
		}
		if (num_read > TWI_DATA_LEN) {
			LOG_ERR("No enough buffer to read %d bytes", num_read);
			return -ENOBUFS;
		}
		err = do_twi_write_read(index, dev_addr,
					twi_data_ascii, (uint16_t)ascii_len,
					num_read);
		if (err < 0) {
			return err;
		}
		break;
	case AT_CMD_TYPE_READ_COMMAND:
		break;
	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTWIWR: <index>,<dev_addr>,<data>,<num_read>\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief API to handle TWI AT commands
 */
int slm_at_twi_parse(const char *at_cmd)
{
	int ret = -ENOENT;
	enum at_cmd_type type;

	for (int i = 0; i < AT_TWI_MAX; i++) {
		if (slm_util_cmd_casecmp(at_cmd, twi_at_list[i].string)) {
			ret = at_parser_params_from_str(at_cmd, NULL,
						&at_param_list);
			if (ret) {
				LOG_ERR("Failed to parse AT command %d", ret);
				return -EINVAL;
			}
			type = at_parser_cmd_type_get(at_cmd);
			ret = twi_at_list[i].handler(type);
			break;
		}
	}

	return ret;
}

/**@brief API to list TWI AT commands
 */
void slm_at_twi_clac(void)
{
	for (int i = 0; i < AT_TWI_MAX; i++) {
		sprintf(rsp_buf, "%s\r\n", twi_at_list[i].string);
		rsp_send(rsp_buf, strlen(rsp_buf));
	}
}

int slm_at_twi_init(void)
{
	return 0;
}

int slm_at_twi_uninit(void)
{
	return 0;
}

