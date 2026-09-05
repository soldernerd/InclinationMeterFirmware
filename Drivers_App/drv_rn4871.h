#ifndef DRV_RN4871_H
#define DRV_RN4871_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Microchip RN4871 BLE module, Transparent UART mode, on USART6 (see
 * hal_uart.c). ~BLE_RESET is PB5 (active low). No MODE/P2_0 line to the
 * MCU on this board — the module self-selects Application mode on power-up
 * and is configured over the UART via the "$$$" command interface.
 *
 * The module is assumed factory-fresh at every boot: drv_rn4871_task()
 * runs a non-blocking state machine that resets it, enters command mode,
 * sets the advertised name + enables the Device-Info + UART-Transparent
 * services (SS,C0), reboots it, and then pipes bytes. Config takes ~1 s of
 * wall time but blocks nothing — it advances one step per task call. */

typedef void (*Rn4871RxCallback)(const uint8_t *data, uint16_t len);

DrvStatus drv_rn4871_init(void);   /* kicks off the reset/config sequence */
void      drv_rn4871_task(void);   /* pump every scheduler tick (task_ble) */

void      drv_rn4871_register_rx_callback(Rn4871RxCallback cb);

/* Module configured and in data mode — usable, whether or not a central
 * is currently connected. False while still configuring, or if config
 * failed (no module, or it never answered). */
bool      drv_rn4871_is_ready(void);

/* A central has an open Transparent UART stream right now. */
bool      drv_rn4871_is_connected(void);

/* Write transparent payload out to the connected central. Fails if not
 * ready or not connected. */
DrvStatus drv_rn4871_send(const uint8_t *data, uint16_t len);

#endif /* DRV_RN4871_H */
