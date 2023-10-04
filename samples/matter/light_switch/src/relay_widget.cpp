/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "relay_widget.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

const struct device *gpio_dev;

void RelayWidget::InitGpio()
{
	int err;

	gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	if (!device_is_ready(gpio_dev)) {
		printk("GPIO controller not ready");
		return;
	}
#if defined (NRF52840_XXAA)
#if CONFIG_NUMBER_OF_RELAY == 4
	err = gpio_pin_configure(gpio_dev, 5, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 5);
		return;
	}
	err = gpio_pin_configure(gpio_dev, 6, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 6);
		return;
	}
#endif
	err = gpio_pin_configure(gpio_dev, 7, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 7);
		return;
	}
	err = gpio_pin_configure(gpio_dev, 8, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 8);
		return;
	}
#elif defined (NRF5340_XXAA)
#if CONFIG_NUMBER_OF_RELAY == 4
	err = gpio_pin_configure(gpio_dev, 6, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 6);
		return;
	}
	err = gpio_pin_configure(gpio_dev, 7, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 7);
		return;
	}
#endif
	err = gpio_pin_configure(gpio_dev, 8, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 9);
		return;
	}
	err = gpio_pin_configure(gpio_dev, 9, GPIO_OUTPUT | GPIO_ACTIVE_HIGH); 
	if (err != 0) {
		printk("Failed to configure pin %d\n", 8);
		return;
	}
#endif
}

void RelayWidget::Init(chip::EndpointId aRelayEndpoint, uint32_t gpioNum)
{
	mRelayEndpoint = aRelayEndpoint;
	mGPIONum = gpioNum;
	Set(false);
}

void RelayWidget::Invert()
{
	Set(!mState);
}

void RelayWidget::Set(bool state)
{
	DoSet(state);
}

void RelayWidget::DoSet(bool state)
{
	mState = state;
	gpio_pin_set(gpio_dev, mGPIONum, mState);
}
