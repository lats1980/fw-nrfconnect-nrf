#include <linux_fmac_main.h>
#include <linux/usb.h>

#include "nrf_wifi_usb.h"

#include <osal_structs.h>
#include <hal_structs.h>
#include <qspi.h>
#include "shim.h"

#define DRV_NAME "nrf_wifi_usb"

extern struct wifi_nrf_drv_priv_lnx rpu_drv_priv_lnx;
static int nrf_usb_probe(struct usb_interface *interface,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct wifi_nrf_rpu_priv_lnx* rpu_priv = NULL;
	int i;
	struct usb_host_interface *iface_desc = interface->cur_altsetting;
	struct usb_endpoint_descriptor *epd;
	struct wifi_nrf_bus_qspi_priv* qspi_priv;
	struct lnx_shim_bus_qspi_priv *linux_qspi_priv = NULL;

	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		epd = &iface_desc->endpoint[i].desc;
		if (usb_endpoint_dir_in(epd)) {
			printk("In ep: %d bulk:%d int:%d\n",  usb_endpoint_num(epd), usb_endpoint_xfer_bulk(epd), usb_endpoint_xfer_int(epd));
		}
		if (usb_endpoint_dir_out(epd)) {
			printk("Out ep: %d bulk:%d int:%d\n",  usb_endpoint_num(epd), usb_endpoint_xfer_bulk(epd), usb_endpoint_xfer_int(epd));
		}
	}

	qspi_priv = (struct wifi_nrf_bus_qspi_priv *)rpu_drv_priv_lnx.fmac_priv->hpriv->bpriv->bus_priv;
	linux_qspi_priv = (struct lnx_shim_bus_qspi_priv *)qspi_priv->os_qspi_priv;
	linux_qspi_priv->usbdev = udev;

	rpu_priv = wifi_nrf_fmac_dev_add_lnx(&interface->dev);
    if (!rpu_priv) {
        printk("%s: failed\n", __func__);
        goto err;
    }
	dev_set_drvdata(&udev->dev, rpu_priv);
	return 0;
err:
	return -1;
}

static void nrf_usb_disconnect(struct usb_interface *interface)
{
	struct wifi_nrf_rpu_priv_lnx* rpu_priv = NULL;
	struct usb_device *udev = interface_to_usbdev(interface);

	printk("nRF WiFi driver disconnect\n");
	rpu_priv = dev_get_drvdata(&udev->dev);
	if (!rpu_priv) {
		printk("Fail to get rpu priv data\n");
	}
	wifi_nrf_fmac_dev_rem_lnx(rpu_priv);
}

static const struct usb_device_id nrf_usb_device_table[] = {
	{
		USB_DEVICE_AND_INTERFACE_INFO(NRF700X_VENDOR_ID,
						  NRF700X_PRODUCT_ID,
						  USB_CLASS_VENDOR_SPEC,
						  0, 0)
	},
	/* end with null element */
	{}
};
MODULE_DEVICE_TABLE(usb, nrf_usb_device_table);

static struct usb_driver nrf_wifi_driver = {
	.name		= DRV_NAME,
	.probe		= nrf_usb_probe,
	.disconnect	= nrf_usb_disconnect,
	.id_table	= nrf_usb_device_table,
};

int nrf_wifi_usb_init(void) {
	int result;

	result = usb_register(&nrf_wifi_driver);
	if (!result) {
		printk(KERN_ERR "loading nRF7002 driver ok\n");
	}
	else {
		printk(KERN_ERR "loading nRF7002 driver failed\n");
	}
	return result;
}

void nrf_wifi_usb_exit(void) {
	usb_deregister(&nrf_wifi_driver);
}

