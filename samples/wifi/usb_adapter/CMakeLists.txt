#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_usb_adapter)

target_include_directories(app PRIVATE
	${ZEPHYR_BASE}/subsys/usb/device
	${WEST_TOPDIR}/nrf/drivers/wifi/nrf700x/zephyr/src/qspi/inc
	${WEST_TOPDIR}/nrf/drivers/wifi/nrf700x/linux/src
)

target_sources(app PRIVATE
	src/main.c
	${WEST_TOPDIR}/nrf/drivers/wifi/nrf700x/zephyr/src/qspi/src/rpu_hw_if.c
)

zephyr_library_sources_ifdef(CONFIG_NRF700X_ON_QSPI
	${WEST_TOPDIR}/nrf/drivers/wifi/nrf700x/zephyr/src/qspi/src/device.c
	${WEST_TOPDIR}/nrf/drivers/wifi/nrf700x/zephyr/src/qspi/src/qspi_if.c
)