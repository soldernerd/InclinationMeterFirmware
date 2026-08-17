#ifndef SVC_USB_H
#define SVC_USB_H

#include <stdint.h>
#include <stdbool.h>

void svc_usb_init(void);
void svc_usb_update(void);
bool svc_usb_is_connected(void);
void svc_usb_send(const uint8_t *data, uint16_t len);

#endif /* SVC_USB_H */
