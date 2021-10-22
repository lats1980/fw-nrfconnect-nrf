/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include "slm_util.h"
#include "slm_at_gpio.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#include "slm_stats.h"
#endif
#if defined(CONFIG_SLM_DIAG)
#include "slm_diag.h"
#endif

LOG_MODULE_REGISTER(slm_gpio, CONFIG_SLM_LOG_LEVEL);

/* global functions defined in different resources */
void rsp_send(const uint8_t *str, size_t len);
int poweron_uart(bool sync_str);
int poweroff_uart(void);

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];

const struct device *gpio_dev;
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
static struct gpio_callback gpio_cb;
#endif
static sys_slist_t slm_gpios = SYS_SLIST_STATIC_INIT(&slm_gpios);

/* global variable defined in different files */
extern struct k_work_q slm_work_q;
static struct k_work gpio_work;

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
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	case SLM_GPIO_FN_RS232_DTR:
		gpio_flags = GPIO_INPUT | GPIO_PULL_UP;
		break;
#endif
#if defined(CONFIG_SLM_UI)
	case SLM_GPIO_FN_LTE:
	case SLM_GPIO_FN_DATA:
	case SLM_GPIO_FN_SIGNAL:
#if defined(CONFIG_SLM_CUSTOMIZED)
	case SLM_GPIO_FN_DIAG:
	case SLM_GPIO_FN_MOD_FLASH:
#endif
		gpio_flags = GPIO_OUTPUT;
		break;
#endif
	default:
		LOG_ERR("Fail to convert gpio flag");
		break;
	}

	return gpio_flags;
}

#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
static void gpio_cb_handler(const struct device *gpio_dev, struct gpio_callback *cb, uint32_t pins)
{
	struct slm_gpio_pin_t *cur = NULL, *next = NULL;
	int err = 0;

	LOG_INF("gpio callback handler on pins: %d", pins);
	/* Trace gpio list */
	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur,
					     next, node) {
			if (BIT(cur->pin) & pins) {
				if (cur->fn == SLM_GPIO_FN_RS232_DTR) {
					LOG_DBG("Find pin: %d as DTR pin", cur->pin);
					err = gpio_pin_interrupt_configure(gpio_dev, cur->pin,
								GPIO_INT_DISABLE);
					if (err) {
						LOG_ERR("Fail to disable interrupt on pin: %d",
							cur->pin);
					}
					k_work_submit_to_queue(&slm_work_q, &gpio_work);
				}
			}
		}
	}
}
#endif

int do_gpio_pin_configure_set(gpio_pin_t pin, uint16_t fn)
{
	int err = 0;
	gpio_flags_t gpio_flags = 0;
	struct slm_gpio_pin_t *slm_gpio_pin = NULL, *cur = NULL, *next = NULL;
	gpio_port_pins_t pin_mask = 0;

	LOG_INF("pin:%hu fn:%hu", pin, fn);

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
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
			if (fn == SLM_GPIO_FN_RS232_DTR) {
				pin_mask |= BIT(cur->pin);
			}
#endif
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

	err = gpio_pin_configure(gpio_dev, pin, gpio_flags);
	if (err) {
		LOG_ERR("GPIO_0 config error: %d", err);
		//TODO: free and remove node
	}

	slm_gpio_pin->pin = pin;
	slm_gpio_pin->fn = fn;

	if (fn == SLM_GPIO_FN_DISABLE) {
		/* Disable interrupt */
		err = gpio_pin_interrupt_configure(gpio_dev, pin, GPIO_INT_DISABLE);
		if (err) {
			LOG_ERR("Interface pin interrupt config error: %d", err);
			return err;
		}
		/* Remove callback */
		pin_mask &= ~BIT(pin);
	}
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	/* Configure GPIO callback for functional GPIO */
	if (fn == SLM_GPIO_FN_RS232_DTR) {
		/* Add gpio callback */
		pin_mask |= BIT(pin);
		gpio_init_callback(&gpio_cb, gpio_cb_handler, pin_mask);
		err = gpio_add_callback(gpio_dev, &gpio_cb);
		if (err) {
			LOG_ERR("Cannot configure cb (pin:%hu)", pin);
			//TODO: free and remove node
		}
		/* Verify pin state and configure interrupt */
		k_work_submit_to_queue(&slm_work_q, &gpio_work);
	}
#endif

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

static void gpio_work_handle(struct k_work *work)
{
	struct slm_gpio_pin_t *cur = NULL, *next = NULL;

	/* Trace gpio list */
	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur,
					     next, node) {
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
			if (cur->fn == SLM_GPIO_FN_RS232_DTR) {
				uint32_t int_conf = GPIO_INT_DISABLE;
				int ret = 0;

				ret = gpio_pin_get(gpio_dev, cur->pin);
				if (ret < 0) {
					LOG_ERR("Cannot read gpio high");
					return;
				}
				if (ret == 0) {
					/* Enable UART if DTR is low state */
					ret = poweron_uart(false);
					int_conf = GPIO_INT_LEVEL_HIGH;
					ui_led_set_state(LED_ID_LTE, UI_UNMUTE);
					ui_led_set_state(LED_ID_DATA, UI_UNMUTE);
					ui_led_set_state(LED_ID_SIGNAL, UI_UNMUTE);
					ui_led_set_state(LED_ID_DIAG, UI_UNMUTE);
					ret = slm_stats_read();
					if (ret != 0) {
						LOG_ERR("Fail to get current stats");
					}
				} else {
					ui_led_set_state(LED_ID_LTE, UI_MUTE);
					ui_led_set_state(LED_ID_DATA, UI_MUTE);
					ui_led_set_state(LED_ID_SIGNAL, UI_MUTE);
					ui_led_set_state(LED_ID_DIAG, UI_MUTE);
					/* Disable UART if DTR is high state */
					ret = poweroff_uart();
					int_conf = GPIO_INT_LEVEL_LOW;
				}
				if (ret) {
					LOG_ERR("Failed to wake up uart: %d", ret);
				}
				ret = gpio_pin_interrupt_configure(gpio_dev, cur->pin,
								int_conf);
				if (ret) {
					LOG_ERR("Fail to set interrupt. pin: %d, %d",
						cur->pin, ret);
				}
			}
#endif
		}
	}
}

#if defined(CONFIG_SLM_UI)
int slm_gpio_get_ui_pin(uint16_t fn)
{
	int ret = -EINVAL;
	struct slm_gpio_pin_t *cur = NULL, *next = NULL;

	/* Trace gpio list */
	if (sys_slist_peek_head(&slm_gpios) != NULL) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&slm_gpios, cur, next, node) {
			if (cur->fn == fn) {
				return (int)cur->pin;
			}
		}
	}

	return ret;
}
#endif

int slm_at_gpio_init(void)
{
	int err = 0;

	gpio_dev = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
	if (gpio_dev == NULL) {
		LOG_ERR("GPIO_0 bind error");
		return -EIO;
	}

#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	err = gpio_pin_configure(gpio_dev, CONFIG_SLM_RI_PIN, GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_RI_PIN config error: %d", err);
		return err;
	}
	err = gpio_pin_configure(gpio_dev, CONFIG_SLM_DCD_PIN, GPIO_OUTPUT_HIGH);
	if (err) {
		LOG_ERR("CONFIG_SLM_DCD_PIN config error: %d", err);
		return err;
	}
#endif

	k_work_init(&gpio_work, gpio_work_handle);

	return err;
}

int slm_at_gpio_uninit(void)
{
	int err = 0;

	return err;
}
