#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* USART6 transport — the link to the RN4871 BLE module (PB8 TX / PB9 RX,
 * 115200 8N1). RX runs on a free-running circular DMA into an internal
 * ring buffer; nothing drives an interrupt, callers just drain with
 * hal_uart_read() from a scheduler task. TX is blocking with a bounded
 * timeout (BLE frames are <= 64 bytes; ~5.5 ms on the wire at 115200).
 *
 * Not the debug/VCP UART (that's USART3, handled elsewhere). */

void      hal_uart_init(void);

/* Blocking, bounded (HAL_MAX_DELAY is never used). Returns DRV_OK once all
 * `len` bytes are handed to the peripheral, DRV_ERR_TIMEOUT / DRV_ERR_COMM
 * otherwise. */
DrvStatus hal_uart_write(const uint8_t *data, uint16_t len);

/* Copies up to `maxlen` bytes out of the RX ring into `dst`; returns the
 * count actually copied (0 if nothing pending). Non-blocking. */
uint16_t  hal_uart_read(uint8_t *dst, uint16_t maxlen);

/* Bytes currently sitting in the RX ring, unread. */
uint16_t  hal_uart_rx_available(void);

/* Discards everything currently in the RX ring. Used around the RN4871
 * command/response handshake to drop stale banner/echo bytes. */
void      hal_uart_rx_flush(void);

#endif /* HAL_UART_H */
