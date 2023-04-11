/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief Header containing OS interface specific declarations for the
 * Linux OS layer of the Wi-Fi driver.
 */

#ifndef __SHIM_H__
#define __SHIM_H__

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
#include <linux/usb.h>
#endif

#include <net/cfg80211.h>

struct nwb {
	unsigned char *data;
	unsigned char *tail;
	int len;
	int headroom;
	void *next;
	void *priv;
	int iftype;
	void *ifaddr;
	void *dev;
	int hostbuffer;
};

#define	USB_INTR_CONTENT_LENGTH		16

struct lnx_shim_intr_priv {
	void *callbk_data;
	int (*callbk_fn)(void *callbk_data);
	struct work_struct work;
};

struct lnx_shim_bus_qspi_priv {
#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
	struct usb_device *usbdev;
	struct urb *urb;
	u8 int_buf[USB_INTR_CONTENT_LENGTH];
#endif
	struct lnx_shim_intr_priv intr_priv;
	bool dev_added;
	bool dev_init;
};

struct lnx_shim_llist_node {
	void *data;
	struct list_head list;
};

struct lnx_shim_llist {
	unsigned int len;
	struct list_head list;
};

struct work_item {
	struct work_struct work;
	unsigned long data;
	void (*callback)(unsigned long data);
};

void *net_pkt_to_nbuf(struct sk_buff *skb);

#endif /* __SHIM_H__ */
