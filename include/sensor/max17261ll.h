/*
 * Copyright (c) 2021 Nordic Semiconductor
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef NRF_DRIVERS_SENSOR_BATTERY_MAX17261LL_H_
#define NRF_DRIVERS_SENSOR_BATTERY_MAX17261LL_H_

#include <zephyr.h>

struct max17261_data {
	const struct device *i2c;
};

struct max17261_config {
	char *bus_name;
};

/*
 * Read operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_read(const struct device *dev, uint8_t reg_addr,
		      uint16_t *val);

/*
 * Write operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_write(const struct device *dev, uint8_t reg_addr,
		       uint16_t val);

/*
 * Verified write operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_write_verify(const struct device *dev, uint8_t reg_addr,
			      uint16_t val);

#endif
