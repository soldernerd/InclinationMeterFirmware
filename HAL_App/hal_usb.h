#ifndef HAL_USB_H
#define HAL_USB_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*HalUsbRxCallback)(const uint8_t *data, uint16_t len);

void hal_usb_init(void);
bool hal_usb_is_connected(void);                       /* VBUS_SENSE PA2, gated on USBD_STATE_CONFIGURED */
bool hal_usb_send(const uint8_t *data, uint16_t len);  /* sends one 64-byte HID IN report */
void hal_usb_register_rx_callback(HalUsbRxCallback cb);

/* Called from USB_Device/App/usbd_custom_hid_if.c's CUSTOM_HID_OutEvent_FS
 * (USER CODE injection) — the ST middleware fires this on every OUT
 * report. Forwards to the registered callback. */
void hal_usb_on_rx(const uint8_t *data, uint16_t len);

void hal_usb_update(void);                              /* poll connection state */

#endif /* HAL_USB_H */
