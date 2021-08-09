/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <net/socket.h>
#include <modem/modem_info.h>
#include <modem/modem_key_mgmt.h>
#include "slm_util.h"
#include "slm_native_tls.h"
#include "slm_at_cmng.h"

LOG_MODULE_REGISTER(cmng, CONFIG_SLM_LOG_LEVEL);

/**@brief List of supported opcode */
enum slm_xcmng_opcode {
	AT_XCMNG_OP_WRITE,
	AT_XCMNG_OP_LIST,
	AT_XCMNG_OP_READ,
	AT_XCMNG_OP_DELETE
};

/**@brief List of supported type */
enum slm_xcmng_type {
	AT_XCMNG_TYPE_CA_CERT,
	AT_XCMNG_TYPE_CERT,
	AT_XCMNG_TYPE_PRIV,
	AT_XCMNG_TYPE_PSK,
	AT_XCMNG_TYPE_PSK_ID,
};

#define MODEM_KEY_MGMT_OP_LS "AT%CMNG=1"

/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];

/**@brief handle AT%CMNG commands
 *  AT%CMNG=<opcode>[,<sec_tag>[,<type>[,<content>]]]
 *  AT%CMNG? READ command not supported
 *  AT%CMNG=? READ command not supported
 */
int handle_at_cmng(enum at_cmd_type cmd_type)
{
	int err = -ENOENT;
	uint16_t op;
	nrf_sec_tag_t sec_tag;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) >= 3) {
			err = at_params_unsigned_int_get(&at_param_list, 2, &sec_tag);
			if (err < 0) {
				LOG_ERR("Fail to get sec_tag parameter: %d", err);
				return -EINVAL;
			}
			if (sec_tag > MAX_MODEM_TLS_SEC_TAG) {
				LOG_ERR("Invalid security tag: %d", sec_tag);
				return -EINVAL;
			} else {
				return -ENOENT;
			}
		} else if (at_params_valid_count_get(&at_param_list) == 2) {
			err = at_params_unsigned_short_get(&at_param_list, 1, &op);
			if (err < 0) {
				LOG_ERR("Fail to get op parameter: %d", err);
				return err;
			}
			if (op == AT_XCMNG_OP_LIST) {
				int written;
				char cmd[32];
				enum at_cmd_state state;

				written = snprintf(cmd, sizeof(cmd), "%s", MODEM_KEY_MGMT_OP_LS);
				if (written < 0 || written >= sizeof(cmd)) {
					return -ENOBUFS;
				}
				err = at_cmd_write(cmd, rsp_buf, CONFIG_AT_CMD_RESPONSE_MAX_LEN, &state);
				if (err) {
					return -EINVAL;
				}
				if (strlen(rsp_buf) > 0) {
					char *ch = rsp_buf;

					/* find sub string %CMNG */
					while( (ch = strstr(ch,"%CMNG: ")) != NULL) {
						char xrsp_buf[32];
						unsigned int ctag = 0, ctype = 0;

						/* remap sec_tag and type */
						sscanf(ch, "%*s%u,%u", &ctag, &ctype);
						if (ctag <= MAX_MODEM_TLS_SEC_TAG) {
							sprintf(xrsp_buf, "%%CMNG: %u,%u\r\n", ctag, ctype);
							rsp_send(xrsp_buf, strlen(xrsp_buf));
						}
						ch = ch + strlen("%CMNG: ");
					}
				}
				return 0;
			}
		}
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XCMNG commands
 *  AT#XCMNG=<opcode>[,<sec_tag>[,<type>[,<content>]]]
 *  AT#XCMNG? READ command not supported
 *  AT#XCMNG=? READ command not supported
 */
int handle_at_xcmng(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op, type;
	nrf_sec_tag_t sec_tag;
	uint8_t *content;
	size_t len = CONFIG_AT_CMD_RESPONSE_MAX_LEN;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) < 2) {
			LOG_ERR("Parameter missed");
			return -EINVAL;
		}
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err < 0) {
			LOG_ERR("Fail to get op parameter: %d", err);
			return err;
		}
		if (op > AT_XCMNG_OP_DELETE) {
			LOG_ERR("Wrong XCMNG operation: %d", op);
			return -EPERM;
		}
		if (op == AT_XCMNG_OP_LIST) {
			int written;
			char cmd[32];
			enum at_cmd_state state;

			written = snprintf(cmd, sizeof(cmd), "%s", MODEM_KEY_MGMT_OP_LS);
			if (written < 0 || written >= sizeof(cmd)) {
				return -ENOBUFS;
			}
			err = at_cmd_write(cmd, rsp_buf, CONFIG_AT_CMD_RESPONSE_MAX_LEN, &state);
			if (err) {
				return -EINVAL;
			}
			if (strlen(rsp_buf) > 0) {
				char *ch = rsp_buf;

				/* find sub string %CMNG */
				while( (ch = strstr(ch,"%CMNG: ")) != NULL) {
					char xrsp_buf[32];
					unsigned int ctag = 0, ctype = 0;

					/* remap sec_tag and type */
					sscanf(ch, "%*s%u,%u", &ctag, &ctype);
					if ((ctype == AT_XCMNG_TYPE_CA_CERT) && (ctag >= MIN_NATIVE_TLS_SEC_TAG)
						&& (ctag <= (MAX_NATIVE_TLS_SEC_TAG + 1) * 10)) {
						sprintf(xrsp_buf, "#XCMNG: %u,%u\r\n", ctag / 10, ctag % 10);
						rsp_send(xrsp_buf, strlen(xrsp_buf));
					}
					ch = ch + strlen("%CMNG: ");
				}
			}
			return 0;
		}
		if (at_params_valid_count_get(&at_param_list) < 4) {
			/* READ, WRITE, DELETE requires sec_tag and type */
			LOG_ERR("Parameter missed");
			return -EINVAL;
		}
		err = at_params_unsigned_int_get(&at_param_list, 2, &sec_tag);
		if (err < 0) {
			LOG_ERR("Fail to get sec_tag parameter: %d", err);
			return err;
		};
		if ((sec_tag < MIN_NATIVE_TLS_SEC_TAG) || (sec_tag > MAX_NATIVE_TLS_SEC_TAG)) {
			LOG_ERR("Invalid security tag: %d", sec_tag);
			return -EINVAL;
		}
		err = at_params_unsigned_short_get(&at_param_list, 3, &type);
		if (err < 0) {
			LOG_ERR("Fail to get type parameter: %d", err);
			return err;
		};
		if (op == AT_XCMNG_OP_WRITE) {
			if (at_params_valid_count_get(&at_param_list) < 5) {
				/* WRITE requires sec_tag, type and content */
				LOG_ERR("Parameter missed");
				return -EINVAL;
			}
			content = k_malloc(CONFIG_AT_CMD_RESPONSE_MAX_LEN);
			err = util_string_get(&at_param_list, 4, content,
						   &len);
			if (err != 0) {
				LOG_ERR("Failed to get content");
				k_free(content);
				return err;
			}
			err = slm_tls_storage_set(sec_tag, type, content, len);
			if (err != 0) {
				LOG_ERR("FAILED! modem_key_mgmt_write() = %d",
					err);
			}
			k_free(content);
		} else if (op == AT_XCMNG_OP_READ) {
			if (type == AT_XCMNG_TYPE_CERT ||
			   type == AT_XCMNG_TYPE_PRIV ||
			   type == AT_XCMNG_TYPE_PSK) {
				/* Not supported */
				LOG_ERR("Not support READ for type: %d", type);
				return -EPERM;
			}
			content = k_malloc(CONFIG_AT_CMD_RESPONSE_MAX_LEN);
			err = slm_tls_storage_get(sec_tag, type, content,
						CONFIG_AT_CMD_RESPONSE_MAX_LEN,
						&len);
			if (err != 0) {
				LOG_ERR("FAILED! modem_key_mgmt_read() = %d",
					err);
			} else {
				*(content + len) = '\0';
				sprintf(rsp_buf, "#XCMNG: %d,%d,\"\","
					"\"%s\"\r\n", sec_tag, type, content);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
			k_free(content);
		} else if (op == AT_XCMNG_OP_DELETE) {
			err = slm_tls_storage_remove(sec_tag, type);
			if (err != 0) {
				LOG_ERR("FAILED! modem_key_mgmt_delete() = %d",
					err);
			}
		}
		break;

	default:
		break;
	}

	return err;
}

/**@brief API to initialize CMNG AT commands handler
 */
int slm_at_cmng_init(void)
{
	return 0;
}

/**@brief API to uninitialize CMNG AT commands handler
 */
int slm_at_cmng_uninit(void)
{
	return 0;
}
