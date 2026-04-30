/* WP1 stub — implemented in WPx */
#include "hal_uart.h"

void      hal_uart_init(void)                              {}
DrvStatus hal_uart_write(const uint8_t *data, uint16_t len){ (void)data; (void)len; return DRV_OK; }
DrvStatus hal_uart_read(uint8_t *data, uint16_t len)       { (void)data; (void)len; return DRV_OK; }
bool      hal_uart_is_busy(void)                           { return false; }
