#include <linux_fmac_main.h>
#include <linux/usb.h>

#include "nrf700x_usb.h"

#include <osal_structs.h>
#include <hal_structs.h>
#include <qspi.h>
#include "shim.h"

extern struct wifi_nrf_drv_priv_linux rpu_drv_priv_linux;
static int nrf700x_probe(struct usb_interface *interface,
			 const struct usb_device_id *id)
{
    enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
    struct usb_device *udev = interface_to_usbdev(interface);
    int i;
    struct usb_host_interface *iface_desc = interface->cur_altsetting;
    struct usb_endpoint_descriptor *epd;
    struct wifi_nrf_bus_qspi_priv* qspi_priv;
	struct linux_shim_bus_qspi_priv *linux_qspi_priv = NULL;

	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		epd = &iface_desc->endpoint[i].desc;
		if (usb_endpoint_dir_in(epd)) {
			printk("In ep: %d bulk:%d int:%d\n",  usb_endpoint_num(epd), usb_endpoint_xfer_bulk(epd), usb_endpoint_xfer_int(epd));
		}
		if (usb_endpoint_dir_out(epd)) {
			printk("Out ep: %d bulk:%d int:%d\n",  usb_endpoint_num(epd), usb_endpoint_xfer_bulk(epd), usb_endpoint_xfer_int(epd));
		}
    }

	qspi_priv = (struct wifi_nrf_bus_qspi_priv *)rpu_drv_priv_linux.fmac_priv->hpriv->bpriv->bus_priv;
	linux_qspi_priv = (struct linux_shim_bus_qspi_priv *)qspi_priv->os_qspi_priv;
	linux_qspi_priv->usbdev = udev;

    status = wifi_nrf_fmac_dev_add_linux();
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_dev_add_linux failed\n", __func__);
		goto err;
	}
    return 0;
err:
    return -1;
}

static void nrf700x_disconnect(struct usb_interface *interface)
{
    printk(KERN_ERR "nRF7002 driver discon\n");
    wifi_nrf_fmac_dev_rem_linux(&rpu_drv_priv_linux);
}

static const struct usb_device_id nrf700x_device_table[] = {
	{
		USB_DEVICE_AND_INTERFACE_INFO(NRF700X_VENDOR_ID,
					      NRF700X_PRODUCT_ID,
					      USB_CLASS_VENDOR_SPEC,
					      0, 0)
	},
	/* end with null element */
	{}
};
MODULE_DEVICE_TABLE(usb, nrf700x_device_table);

static struct usb_driver nrf700x_driver = {
	.name		= "nrf700x",
	.probe		= nrf700x_probe,
	.disconnect	= nrf700x_disconnect,
	.id_table	= nrf700x_device_table,
};

int nrf700x_usb_init(void) {
	int result;

	result = usb_register(&nrf700x_driver);
    if (!result) {
        printk(KERN_ERR "loading nRF7002 driver ok\n");
    }
    else {
        printk(KERN_ERR "loading nRF7002 driver failed\n");
    }
    return result;
}

void nrf700x_usb_exit(void) {
    usb_deregister(&nrf700x_driver);
}

