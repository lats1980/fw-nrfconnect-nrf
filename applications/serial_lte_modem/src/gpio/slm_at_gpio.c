/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <drivers/gpio.h>
#include "slm_util.h"
#include "slm_at_gpio.h"

LOG_MODULE_REGISTER(slm_gpio, CONFIG_SLM_LOG_LEVEL);

#define MAX_GPIO_PIN 31

#define SLM_GPIO_FN_DISABLE	0	/* Disables pin for both input and output. */
#define SLM_GPIO_FN_OUT		1	/* Enables pin as output. */
#define SLM_GPIO_FN_IN_PU	21	/* Enables pin as input. Use internal pull up resistor. */
#define SLM_GPIO_FN_IN_PD	22	/* Enables pin as input. Use internal pull down resistor. */

/* global functions defined in different resources */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_SLM_SOCKET_RX_MAX * 2];

static const struct device *gpio_dev;
static sys_slist_t slm_gpios = SYS_SLIST_STATIC_INIT(&slm_gpios);

/**@brief GPIO operations. */
enum slm_gpio_operations {
	SLM_GPIO_OP_WRITE,
	SLM_GPIO_OP_READ,
	SLM_GPIO_OP_TOGGLE
};

struct slm_gpio_pin_t {
	sys_snode_t node;
	gpio_pin_t pin;
	uint16_t fn;
};

gpio_flags_t convert_flags(uint16_t fn)
{
	gpio_flags_t gpio_flags = UINT32_MAX;

	switch (fn) {
	case SLM_GPIO_FN_DISABLE:
		gpio_flags = GPIO_DISCONNECTED;
		break;
	case SLM_GPIO_FN_OUT:
		gpio_flags = GPIO_OUTPUT;
		break;
	case SLM_GPIO_FN_IN_PU:
		gpio_flags = GPIO_INPUT | GPIO_PULL_UP;
		break;
	case SLM_GPIO_FN_IN_PD:
		gpio_flags = GPIO_INPUT | GPIO_PULL_DOWN;
		break;
	default:
		LOG_ERR("Fail to convert gpio flag");
		break;
	}

	return gpio_flags;
}

int do_gpio_pin_configure_set(gpio_pin_t pin, uint16_t fn)
{
	int err = 0;
	gpio_flags_t gpio_flags = 0;
	struct slm_gpio_pin_t *slm_gpio_pin = NULL, *cur = NULL, *next = NULL;

	LOG_DBG("pin:%hu fn:%hu", pin, fn);

	/* Verify pin correctness */
	if (pin > MAX_GPIO_PIN) {
		LOG_ERR("Incorrect <pin>: %d", pin);
		return -EINVAL;
	}

	/* Convert SLM GPIO flag to zephyr gpio pin configuration flag */
	gpio_flags = convert_flags(fn);
	if (gpio_flags == UINT32_MAX) {
		LOG_ERR("Fail to configure pin.");
		return -EINVAL;
	}

	/* Trace gpio list */
	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur,
					     next, node) {
			if (cur->pin == pin) {
				slm_gpio_pin = cur;
			}
		}
	}

	/* Add GPIO node if node does not exist */
	if (slm_gpio_pin == NULL) {
		slm_gpio_pin = (struct slm_gpio_pin_t *)k_malloc(sizeof(struct slm_gpio_pin_t));
		if (slm_gpio_pin == NULL) {
			return -ENOBUFS;
		}
		memset(slm_gpio_pin, 0, sizeof(struct slm_gpio_pin_t));
		sys_slist_append(&slm_gpios, &slm_gpio_pin->node);
	}

	LOG_INF("Configure pin: %d with flags: %X", pin, gpio_flags);
	err = gpio_pin_configure(gpio_dev, pin, gpio_flags);
	if (err) {
		LOG_ERR("GPIO_0 config error: %d", err);
		//TODO: free and remove node
	}

	slm_gpio_pin->pin = pin;
	slm_gpio_pin->fn = fn;

	return err;
}

int do_gpio_pin_configure_read(void)
{
	int err = 0;
	struct slm_gpio_pin_t *cur = NULL, *next = NULL;

	sprintf(rsp_buf, "\r\n#XGPIOC\r\n");
	rsp_send(rsp_buf, strlen(rsp_buf));

	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur,
					     next, node) {
			if (cur) {
				LOG_DBG("%hu,%hu", cur->pin, cur->fn);
				sprintf(rsp_buf, "%hu,%hu\r\n", cur->pin, cur->fn);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
		}
	}

	return err;
}

int do_gpio_pin_operate(uint16_t op, gpio_pin_t pin, uint16_t value)
{
	int ret = 0;
	struct slm_gpio_pin_t *cur = NULL, *next = NULL;

	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur,
					     next, node) {
			if (cur) {
				if (cur->pin != pin) {
					continue;
				}
				if (op == SLM_GPIO_OP_WRITE) {
					LOG_DBG("Write pin: %d with value: %d", cur->pin, value);
					ret = gpio_pin_set(gpio_dev, pin, value);
					if (ret < 0) {
						LOG_ERR("Cannot write gpio");
						return ret;
					}
				} else if (op == SLM_GPIO_OP_READ) {
					ret = gpio_pin_get(gpio_dev, pin);
					if (ret < 0) {
						LOG_ERR("Cannot read gpio high");
						return ret;
					}
					LOG_DBG("Read value: %d", ret);
					sprintf(rsp_buf, "\r\n#XGPIO: %d,%d\r\n", pin, ret);
					rsp_send(rsp_buf, strlen(rsp_buf));
				} else if (op == SLM_GPIO_OP_TOGGLE) {
					LOG_DBG("Toggle pin: %d", cur->pin);
					ret = gpio_pin_toggle(gpio_dev, pin);
					if (ret < 0) {
						LOG_ERR("Cannot toggle gpio");
						return ret;
					}
				}
			}
		}
	}

	return 0;
}

/**@brief handle AT#XGPIOC commands
 *  AT#XGPIOC=<pin>,<function>
 *  AT#XGPIOC?
 *  AT#XGPIOC=?
 */
int handle_at_gpio_configure(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t pin = 0, fn = 0;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) == 0) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &pin);
		if (err < 0) {
			LOG_ERR("Fail to get pin: %d", err);
			return err;
		}
		err = at_params_short_get(&at_param_list, 2, &fn);
		if (err < 0) {
			LOG_ERR("Fail to get fn: %d", err);
			return err;
		}
		err = do_gpio_pin_configure_set((gpio_pin_t)pin, fn);
		break;
	case AT_CMD_TYPE_READ_COMMAND:
		err = do_gpio_pin_configure_read();
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XGPIOC commands
 *  AT#XGPIO=<op>,<pin>[,<value>]
 *  AT#XGPIO? READ command not supported
 *  AT#XGPIO=?
 */
int handle_at_gpio_operate(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t pin = 0, op = 0, value = 0;

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
		if (op > SLM_GPIO_OP_TOGGLE) {
			LOG_ERR("Fail to operate gpio: %d", op);
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 2, &pin);
		if (err < 0) {
			LOG_ERR("Fail to get pin: %d", err);
			return err;
		}
		if (pin > MAX_GPIO_PIN) {
			LOG_ERR("Incorrect <pin>: %d", pin);
			return -EINVAL;
		}
		if (at_params_valid_count_get(&at_param_list) == 4) {
			if (op == SLM_GPIO_OP_WRITE) {
				err = at_params_short_get(&at_param_list, 3, &value);
				if (err < 0) {
					LOG_ERR("Fail to get value: %d", err);
					return err;
				}
				if (value != 1 && value != 0) {
					LOG_ERR("Fail to set gpio value: %d", value);
					return -EINVAL;
				}
			}
		}
		err = do_gpio_pin_operate(op, (gpio_pin_t)pin, value);
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

int slm_at_gpio_init(void)
{
	int err = 0;

	gpio_dev = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
	if (gpio_dev == NULL) {
		LOG_ERR("GPIO_0 bind error");
		err = -EIO;
	}

	return err;
}

int slm_at_gpio_uninit(void)
{
	int err = 0;

	return err;
}
