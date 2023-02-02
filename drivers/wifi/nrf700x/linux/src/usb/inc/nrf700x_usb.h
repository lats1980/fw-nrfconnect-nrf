#ifndef NRF700X_USB_H
#define NRF700X_USB_H

#define NRF700X_VENDOR_ID	0x2fe3
#define NRF700X_PRODUCT_ID	0x000d

int nrf700x_usb_init(void);
void nrf700x_usb_exit(void);

#endif /* NRF700X_USB_H */
