#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    HAL_I2C_MAIN = 0,   /* I2C1 — EEPROM (and PCAP04 #1 in later WPs) */
    HAL_I2C_AUX  = 1,   /* I2C2 — reserved, stub in WP2 */
    HAL_I2C_COUNT
} HalI2cInstance;

typedef void (*HalI2cDmaCallback)(HalI2cInstance instance, bool success);

void hal_i2c_init(HalI2cInstance instance);

/* Blocking primitives */
DrvStatus hal_i2c_write(HalI2cInstance instance, uint8_t addr,
                        const uint8_t *data, uint16_t len);
DrvStatus hal_i2c_read(HalI2cInstance instance, uint8_t addr,
                       uint8_t *data, uint16_t len);
DrvStatus hal_i2c_write_read(HalI2cInstance instance, uint8_t addr,
                             const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_len);

/* Non-blocking DMA primitives — completion fires registered callback */
void hal_i2c_read_dma(HalI2cInstance instance, uint8_t addr,
                      uint8_t *buf, uint16_t len);
void hal_i2c_write_dma(HalI2cInstance instance, uint8_t addr,
                       const uint8_t *buf, uint16_t len);
void hal_i2c_register_dma_callback(HalI2cInstance instance,
                                   HalI2cDmaCallback cb);
bool hal_i2c_is_busy(HalI2cInstance instance);

/* True if device at addr ACKs an empty write (used to poll EEPROM
 * write-cycle completion, t_WC up to 5 ms). */
bool hal_i2c_device_ready(HalI2cInstance instance, uint8_t addr);

/* Aborts an in-flight DMA transfer (best-effort — issues a NACK+STOP via
 * the HAL). Use when a caller has given up waiting (e.g. a blocking
 * helper's own timeout) and can no longer guarantee its DMA target buffer
 * stays valid; without this a late completion callback could still write
 * into memory the caller has since reused (e.g. a stack buffer that's
 * gone out of scope). */
void hal_i2c_abort(HalI2cInstance instance, uint8_t addr);

#endif /* HAL_I2C_H */
