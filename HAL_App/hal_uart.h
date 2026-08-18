#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    HAL_UART_BLE   = 0,   /* USART6 <-> RN4871 (WP5) */
    HAL_UART_DEBUG = 1,   /* USART3 <-> STLINK VCP header, PD8/PD9 (WP5.1) */
    HAL_UART_COUNT,
} HalUartInstance;

bool      hal_uart_init(HalUartInstance instance);
DrvStatus hal_uart_write(HalUartInstance instance, const uint8_t *data, uint16_t len);
bool      hal_uart_read_byte(HalUartInstance instance, uint8_t *byte);
uint16_t  hal_uart_bytes_available(HalUartInstance instance);
bool      hal_uart_is_busy(HalUartInstance instance);

#endif /* HAL_UART_H */
