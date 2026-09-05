#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Two DMA UART instances, one engine:
 *   HAL_UART_BLE   — USART6, PB8 TX / PB9 RX, link to the RN4871 BLE module
 *   HAL_UART_DEBUG — USART3, PD8 TX / PD9 RX, the STDC14 / VCP debug header
 * Both 115200 8N1. CubeMX has already set both peripherals and their DMA
 * channels up (Core/Src/usart.c); this layer only drives them.
 *
 * RX is a free-running circular DMA into an internal ring buffer — nothing
 * raises an interrupt we service, callers just drain with hal_uart_read()
 * from a scheduler task. (hal_uart_init() forces the RX DMA to circular
 * mode at runtime so it does not matter whether the .ioc left that
 * channel Normal or Circular.)
 *
 * TX is non-blocking: hal_uart_write() starts a one-shot DMA of the
 * caller's buffer and returns immediately; hal_uart_tx_idle() reports
 * when it has drained. Only one TX transfer is in flight per instance.
 * No UART or DMA interrupt is enabled on either instance — the shared
 * USART3_4_5_6_LPUART1 IRQ handler finds nothing to do. */

typedef enum {
    HAL_UART_BLE   = 0,
    HAL_UART_DEBUG = 1,
    HAL_UART_COUNT,
} HalUartInstance;

void      hal_uart_init(HalUartInstance inst);

/* Start a DMA transmission of `len` bytes from `data`. Non-blocking.
 *
 * OWNERSHIP: `data` is NOT copied — it must stay valid and unmodified
 * until hal_uart_tx_idle(inst) returns true again. Callers pass either a
 * string literal (.rodata) or a static staging buffer they do not touch
 * until the transfer completes (they gate their next write on
 * hal_uart_tx_idle()), so this holds by construction.
 *
 * Returns DRV_OK if the transfer was started, DRV_ERR_NOT_READY if a
 * previous transfer is still in flight (caller retries later),
 * DRV_ERR_INVALID on bad arguments or `len` past the DMA count limit. */
DrvStatus hal_uart_write(HalUartInstance inst, const uint8_t *data, uint16_t len);

/* True when no TX DMA is in flight on this instance (the last byte may
 * still be shifting out of the UART, which does not matter for starting
 * the next transfer — the DMA paces itself on TXE). */
bool      hal_uart_tx_idle(HalUartInstance inst);

/* Copy up to `maxlen` bytes out of the RX ring into `dst`; returns the
 * count actually copied (0 if nothing pending). Non-blocking. */
uint16_t  hal_uart_read(HalUartInstance inst, uint8_t *dst, uint16_t maxlen);

/* Bytes currently sitting unread in the RX ring. */
uint16_t  hal_uart_rx_available(HalUartInstance inst);

/* Discard everything currently in the RX ring. Used around the RN4871
 * command/response handshake to drop stale banner/echo bytes. */
void      hal_uart_rx_flush(HalUartInstance inst);

#endif /* HAL_UART_H */
