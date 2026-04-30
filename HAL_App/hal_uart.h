#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

void      hal_uart_init(void);
DrvStatus hal_uart_write(const uint8_t *data, uint16_t len);
DrvStatus hal_uart_read(uint8_t *data, uint16_t len);
bool      hal_uart_is_busy(void);

#endif /* HAL_UART_H */
