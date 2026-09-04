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

/* ---- temporary bring-up diagnostics (WP4) ----
 * Live snapshot of the USB block, rendered on the STATUS screen so USB
 * enumeration can be debugged without a probe. Remove once USB is up. */
typedef struct {
    uint8_t  attached;      /* our VBUS-gated attach latch (0/1)            */
    uint8_t  vbus_pin;      /* raw VBUS_SENSE (PA2) level (0/1)             */
    uint8_t  dev_state;     /* hUsbDeviceFS.dev_state: 1=DEFAULT 2=ADDR     */
                            /*   3=CONFIGURED 4=SUSPENDED                   */
    uint8_t  dppu;          /* USB_DRD_FS->BCDR DPPU bit (D+ pull-up, 0/1)  */
    uint16_t fnr;           /* USB_DRD_FS->FNR (frame number — counts up    */
                            /*   only while SOF packets are being received) */
    uint32_t crs_isr;       /* CRS->ISR (bit0 SYNCOKF, 1 SYNCWARN,          */
                            /*   2 SYNCERR, 3 SYNCMISS, 4 TRIMOVF)          */
    uint32_t irq_count;     /* USB_UCPD1_2 IRQ entries since boot           */
} HalUsbDebug;

void hal_usb_get_debug(HalUsbDebug *out);
void hal_usb_isr_tick(void);   /* call at the top of USB_UCPD1_2_IRQHandler */

#endif /* HAL_USB_H */
