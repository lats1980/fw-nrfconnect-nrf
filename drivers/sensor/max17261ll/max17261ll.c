/*
 * Copyright (c) 2021 Nordic Semiconductor
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <drivers/i2c.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(max17261ll, CONFIG_SENSOR_LOG_LEVEL);

#include <sensor/max17261ll.h>

#define DT_DRV_COMPAT maxim_max17261

/*
 * Read operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_read(const struct device *dev, uint8_t reg_addr, uint16_t *val)
{
	struct max17261_data *data = dev->data;
	uint8_t i2c_data[2];
	int err;

	err = i2c_burst_read(data->i2c, DT_INST_REG_ADDR(0), reg_addr, i2c_data,
			     2);
	if (err < 0) {
		LOG_ERR("Error reading register");
		return err;
	}
	*val = (i2c_data[1] << 8) | i2c_data[0];

	return 0;
}

/*
 * Write operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_write(const struct device *dev, uint8_t reg_addr, uint16_t val)
{
	struct max17261_data *data = dev->data;
	uint8_t i2c_data[2];
	int err;

	i2c_data[0] = (uint8_t) val;
	i2c_data[1] = (uint8_t) (val >> 8);

	err = i2c_burst_write(data->i2c, DT_INST_REG_ADDR(0), reg_addr,
			      i2c_data, 2);
	if (err < 0) {
		LOG_ERR("Error writing register");
		return err;
	}

	return 0;
}

/*
 * Verified write operation conforming to 
 * https://pdfserv.maximintegrated.com/en/an/MAX1726x-Software-Implementation-user-guide.pdf
 */
int max17261_reg_write_verify(const struct device *dev, uint8_t reg_addr, uint16_t val)
{
	uint16_t val_read;
	int err;

	for (int i = 0; i < 3; i++) {
		err = max17261_reg_write(dev, reg_addr, val);
		if (err < 0) {
			LOG_ERR("Error writing register in verified write");
			return err;
		}
		k_sleep(K_MSEC(1));
		err = max17261_reg_read(dev, reg_addr, &val_read);
		if (err < 0) {
			LOG_ERR("Error reading register in verified write");
			return err;
		}
		if (val_read == val) {
			return 0;
		}
	}
	LOG_ERR("Could not verify write");
	return -EIO;
}

static int max17261_init(const struct device *dev)
{
	struct max17261_data *data = dev->data;
	const struct max17261_config *const config = dev->config;

	data->i2c = device_get_binding(config->bus_name);
	if (!data->i2c) {
		LOG_ERR("Could not get pointer to %s device", config->bus_name);
		return -EINVAL;
	}

	return 0;
}

#define MAX17261_INIT(index)						\
	static struct max17261_data max17261_driver_##index;		\
									\
	static const struct max17261_config max17261_config_##index = { \
		.bus_name = DT_INST_BUS_LABEL(index),			\
	};								\
									\
	DEVICE_INIT(max17261_##index, DT_INST_LABEL(index),	\
		    &max17261_init, &max17261_driver_##index, \
		    &max17261_config_##index, POST_KERNEL,	\
		    CONFIG_SENSOR_INIT_PRIORITY)

DT_INST_FOREACH_STATUS_OKAY(MAX17261_INIT);
