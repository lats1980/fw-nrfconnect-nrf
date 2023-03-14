/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief WiFi usb adapter sample
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wifi_nrf, CONFIG_LOG_DEFAULT_LEVEL);

#include <nrfx_clock.h>

#include <zephyr/usb/usb_device.h>
#include <usb_descriptor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "qspi_if.h"
#include "usb_request.h"

#if IS_ENABLED(CONFIG_NET_TC_THREAD_COOPERATIVE)
#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
#else
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)
#endif

#define WIFI_OUT_EP_ADDR	0x01
#define WIFI_IN_EP_ADDR		0x81
#define WIFI_INT_IN_EP_ADDR	0x83

#define WIFI_OUT_EP_IDX		0
#define WIFI_IN_EP_IDX		1
#define WIFI_INT_IN_EP_IDX	2

#define TX_BUFFER_SIZE		1024

static uint32_t tx_offset;
static uint32_t tx_count;
static uint8_t tx_buf[TX_BUFFER_SIZE];
static uint8_t rx_buf[CONFIG_WIFI_BULK_EP_MPS];
static struct k_fifo tx_queue;

struct rpu_request_item_t {
	void *fifo_reserved;
	uint8_t data[CONFIG_WIFI_BULK_EP_MPS];
};

static atomic_t reg_ready;
static uint32_t reg_val;

/**
 * Stack for the tx thread.
 */
static K_THREAD_STACK_DEFINE(tx_stack, 1024);
static struct k_thread tx_thread_data;

void usb_transfer_handler(struct k_work *work);
K_WORK_DEFINE(usb_transfer_work, usb_transfer_handler);

void usb_int_transfer_handler(struct k_work *work);
K_WORK_DEFINE(usb_int_transfer_work, usb_int_transfer_handler);

static struct gpio_callback gpio_cb_data;

static void wifi_int_cb(uint8_t ep, int size, void *priv)
{
	LOG_DBG("write ep %x size %d", ep, size);
}

static void wifi_write_cb(uint8_t ep, int size, void *priv)
{
	//LOG_INF("write ep %x size %d", ep, size);
	if (size <= 0) {
		goto restart_out_transfer;
	}
	tx_offset += size;
	if (tx_offset >= tx_count) {
		LOG_DBG("Write finished");
		return;
	}
	//len = ((tx_count - tx_offset) > CONFIG_WIFI_BULK_EP_MPS)?CONFIG_WIFI_BULK_EP_MPS:(tx_count - tx_offset);
restart_out_transfer:
	k_work_submit(&usb_transfer_work);
}

static void wifi_read_cb(uint8_t ep, int size, void *priv)
{
	struct rpu_request_item_t *req_item;

	//LOG_INF("read ep %x size %d", ep, size);
	if (size <= 0) {
		goto restart_out_transfer;
	}

	req_item = k_malloc(sizeof(*req_item));
	if (req_item) {
		memcpy(req_item->data, rx_buf, size);
		k_fifo_put(&tx_queue, req_item);
	} else {
		LOG_ERR("Fail to allocate buffer");
	}

restart_out_transfer:
	usb_transfer(ep, rx_buf, sizeof(rx_buf), USB_TRANS_READ,
		     wifi_read_cb, NULL);
}

/* usb.rst config structure start */
struct usb_wifi_ep_config {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_out_ep;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_int_in_ep;
} __packed;

USBD_CLASS_DESCR_DEFINE(primary, 0) struct usb_wifi_ep_config adapter_cfg = {
	/* Interface descriptor 0 */
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 3,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},

	/* Data Endpoint OUT */
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = WIFI_OUT_EP_ADDR,
		.bmAttributes = USB_DC_EP_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(CONFIG_WIFI_BULK_EP_MPS),
		.bInterval = 0x00,
	},

	/* Data Endpoint IN */
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = WIFI_IN_EP_ADDR,
		.bmAttributes = USB_DC_EP_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(CONFIG_WIFI_BULK_EP_MPS),
		.bInterval = 0x00,
	},

	/* Interrupt Endpoint IN */
	.if0_int_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x83,
		.bmAttributes = USB_EP_TYPE_INTERRUPT,
		.wMaxPacketSize = sys_cpu_to_le16(16),
		.bInterval = 0x0a,
	},		
};
/* usb.rst config structure end */

/* usb.rst endpoint configuration start */
static struct usb_ep_cfg_data ep_cfg[] = {
	{
		.ep_cb = usb_transfer_ep_callback,
		.ep_addr = WIFI_OUT_EP_ADDR,
	},
	{
		.ep_cb = usb_transfer_ep_callback,
		.ep_addr = WIFI_IN_EP_ADDR,
	},
	{
		.ep_cb = usb_transfer_ep_callback,
		.ep_addr = WIFI_INT_IN_EP_ADDR,
	},
};

static void adapter_status_cb(struct usb_cfg_data *cfg,
			      enum usb_dc_status_code status,
			      const uint8_t *param)
{
	ARG_UNUSED(param);
	ARG_UNUSED(cfg);

	/* Check the USB status and do needed action if required */
	switch (status) {
	case USB_DC_ERROR:
		LOG_INF("USB device error");
		break;
	case USB_DC_RESET:
		LOG_INF("USB device reset detected");
		break;
	case USB_DC_CONNECTED:
		LOG_INF("USB device connected");
		break;
	case USB_DC_CONFIGURED:
		LOG_INF("USB device configured");
		wifi_read_cb(ep_cfg[WIFI_OUT_EP_IDX].ep_addr,
				0, NULL);
		break;
	case USB_DC_DISCONNECTED:
		LOG_INF("USB device disconnected");
		break;
	case USB_DC_SUSPEND:
		LOG_INF("USB device suspended");
		break;
	case USB_DC_RESUME:
		LOG_INF("USB device resumed");
		break;
	case USB_DC_UNKNOWN:
	default:
		LOG_INF("USB unknown state");
		break;
		}
}

/**
 * Vendor handler is executed in the ISR context, queue data for
 * later processing
 */
static int wifi_vendor_handler(struct usb_setup_packet *setup,
				  int32_t *len, uint8_t **data)
{
	LOG_DBG("Control msg: %u To host: %d", setup->bRequest, usb_reqtype_is_to_host(setup));

	if (usb_reqtype_is_to_host(setup)) {
		if (setup->bRequest == REGISTER_READ) {
			if (!atomic_get(&reg_ready)) {
				LOG_DBG("reg not ready");
				*len = 0;
			} else {
				*len = sizeof(reg_val);
				*data = &reg_val;
				LOG_DBG("r reg ready: %u", *(uint32_t *)*data);
				atomic_set(&reg_ready, false);
			}
			return 0;
		}
		return -ENOTSUP;
	}

	if (setup->bRequest == RPU_ENABLE) {
		struct rpu_request_item_t *req_item;
		struct rpu_request req;

		req.cmd = RPU_ENABLE;
		req_item = k_malloc(sizeof(*req_item));
		if (req_item) {
			memcpy(req_item->data, &req, sizeof(req));
			k_fifo_put(&tx_queue, req_item);
		} else {
			LOG_ERR("Fail to allocate buffer");
		}
		return 0;
	} else if (setup->bRequest == IRQ_ENABLE) {
		struct rpu_request_item_t *req_item;
		struct rpu_request req;

		req.cmd = IRQ_ENABLE;
		req_item = k_malloc(sizeof(*req_item));
		if (req_item) {
			memcpy(req_item->data, &req, sizeof(req));
			k_fifo_put(&tx_queue, req_item);
		} else {
			LOG_ERR("Fail to allocate buffer");
		}
		return 0;
	} else if (setup->bRequest == REGISTER_READ) {
		struct rpu_request_item_t *req_item;

		req_item = k_malloc(sizeof(*req_item));
		if (req_item) {
			memcpy(req_item->data, *data, *len);
			k_fifo_put(&tx_queue, req_item);
		} else {
			LOG_ERR("Fail to allocate buffer");
		}
		return 0;
	} else if (setup->bRequest == REGISTER_WRITE) {
		struct rpu_request_item_t *req_item;

		req_item = k_malloc(sizeof(*req_item));
		if (req_item) {
			memcpy(req_item->data, *data, *len);
			k_fifo_put(&tx_queue, req_item);
		} else {
			LOG_ERR("Fail to allocate buffer");
		}
		return 0;
	}

	return 0;
}

static void adapter_interface_config(struct usb_desc_header *head,
				      uint8_t bInterfaceNumber)
{
	ARG_UNUSED(head);

	adapter_cfg.if0.bInterfaceNumber = bInterfaceNumber;
}

/* usb.rst device config data start */
USBD_DEFINE_CFG_DATA(wifi_ep_config) = {
	.usb_device_description = NULL,
	.interface_config = adapter_interface_config,
	.interface_descriptor = &adapter_cfg.if0,
	.cb_usb_status = adapter_status_cb,
	.interface = {
		.class_handler = NULL,
		.custom_handler = NULL,
		.vendor_handler = wifi_vendor_handler,
	},
	.num_endpoints = ARRAY_SIZE(ep_cfg),
	.endpoint = ep_cfg,
};
/* usb.rst device config data end */

static void irq_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	LOG_DBG("Got interrupt");
	k_work_submit(&usb_int_transfer_work);
}

void usb_transfer_handler(struct k_work *work)
{
	uint32_t len;
	int ret;

	if(usb_transfer_is_busy(ep_cfg[WIFI_IN_EP_IDX].ep_addr)) {
		k_work_submit(&usb_transfer_work);
		return;
	}

	len = ((tx_count - tx_offset) > CONFIG_WIFI_BULK_EP_MPS)?CONFIG_WIFI_BULK_EP_MPS:(tx_count - tx_offset);
	if (!(len % CONFIG_WIFI_BULK_EP_MPS)) {
		len -= 1;
	}
	ret = usb_transfer(ep_cfg[WIFI_IN_EP_IDX].ep_addr, tx_buf + tx_offset, len, USB_TRANS_WRITE, wifi_write_cb, NULL);
	if (ret == -EAGAIN) {
		LOG_INF("USB write again");
		k_work_submit(&usb_transfer_work);
	}
}

void usb_int_transfer_handler(struct k_work *work)
{
	uint32_t val;
	int ret;

	if(usb_transfer_is_busy(wifi_ep_config.endpoint[WIFI_INT_IN_EP_IDX].ep_addr)) {
		k_work_submit(&usb_int_transfer_work);
		return;
	}

	ret = usb_transfer(wifi_ep_config.endpoint[WIFI_INT_IN_EP_IDX].ep_addr, (uint8_t *)&val, sizeof(val), USB_TRANS_WRITE, wifi_int_cb, NULL);
	if (ret == -EAGAIN) {
		LOG_INF("USB int write again");
		k_work_submit(&usb_int_transfer_work);
	}
}

static void tx_thread(void)
{
	LOG_INF("Tx thread started");

	while (1) {
		struct rpu_request_item_t *req_item;

		req_item = k_fifo_get(&tx_queue, K_FOREVER);
		if (req_item) {
			struct rpu_request *req;

			req = (struct rpu_request *)req_item->data;
			LOG_DBG("Get cmd: %u", req->cmd);

			if (req->cmd == RPU_ENABLE) {
				rpu_enable();
			} else if (req->cmd == IRQ_ENABLE) {
				int ret;

				ret = rpu_irq_config(&gpio_cb_data, irq_handler);
				if (ret) {
					LOG_ERR("Enable IRQ failed");
				}
			} else if (req->cmd == REGISTER_READ) {
				if (!atomic_get(&reg_ready)) {
					if (req->read_reg.addr < 0x0C0000) {
						qspi_hl_read(req->read_reg.addr, &reg_val, 4);
					} else {
						qspi_read(req->read_reg.addr, &reg_val, 4);
					}
				} else {
					LOG_INF("register not retrive yet");
					k_fifo_put(&tx_queue, req_item);
					k_sleep(K_MSEC(2));
					continue;
				}
				LOG_DBG("Read register from: %u Got value: %u", req->read_reg.addr, reg_val);
				atomic_set(&reg_ready, true);
			} else if (req->cmd == REGISTER_WRITE) {
				qspi_write(req->write_reg.addr, &req->write_reg.val, 4);
				LOG_DBG("rw: %u %u", req->write_reg.addr, req->write_reg.val);
			} else if (req->cmd == BLOCK_READ) {
				int ret;

				LOG_DBG("cf:%lu %d", req->read_block.addr, req->read_block.count);
				if (req->read_block.count % 4 != 0) {
					req->read_block.count = (req->read_block.count + 4) & 0xfffffffc;
				}
				if (req->read_block.count > TX_BUFFER_SIZE) {
					LOG_ERR("Not enought TX buffer");
				}

				if (req->read_block.addr < 0x0C0000) {
					qspi_hl_read(req->read_block.addr, tx_buf, req->read_block.count );
				} else {
					qspi_read(req->read_block.addr, tx_buf, req->read_block.count );
				}
				LOG_HEXDUMP_DBG(tx_buf, req->read_block.count, "copy from");
				tx_offset = 0;
				tx_count = req->read_block.count;
				k_work_submit(&usb_transfer_work);
			} else if (req->cmd == BLOCK_WRITE) {
				LOG_DBG("ct:%lu %d", req->write_block.addr, req->write_block.count);
				if (req->write_block.count % 4 != 0) {
					req->write_block.count = (req->write_block.count + 4) & 0xfffffffc;
				}
				LOG_HEXDUMP_DBG(req_item->data + sizeof(*req), req->write_block.count, "copy to");
				qspi_write(req->write_block.addr, req_item->data + sizeof(*req), req->write_block.count);
			}
			k_free(req_item);
		}

		k_yield();
	}
}



static void init_tx_queue(void)
{
	/* Transmit queue init */
	k_fifo_init(&tx_queue);

	k_thread_create(&tx_thread_data, tx_stack,
			K_THREAD_STACK_SIZEOF(tx_stack),
			(k_thread_entry_t)tx_thread,
			NULL, NULL, NULL, THREAD_PRIORITY, 0, K_NO_WAIT);
}

void main(void)
{
	int ret;

	atomic_set(&reg_ready, false);
#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
	/* For now hardcode to 128MHz */
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK,
			       NRF_CLOCK_HFCLK_DIV_1);
#endif
	//k_sleep(K_SECONDS(1));
	LOG_INF("Starting %s with CPU frequency: %d MHz", CONFIG_BOARD, SystemCoreClock / MHZ(1));

	/* Initialize transmit queue */
	init_tx_queue();

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return;
	}
}
