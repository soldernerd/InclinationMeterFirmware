#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    HAL_I2C_MAIN = 0,
    HAL_I2C_AUX  = 1,
} HalI2cInstance;

typedef void (*HalI2cDmaCallback)(HalI2cInstance instance, bool success);

void      hal_i2c_init(HalI2cInstance instance);
DrvStatus hal_i2c_write(HalI2cInstance instance, uint8_t addr, const uint8_t *data, uint16_t len);
DrvStatus hal_i2c_read(HalI2cInstance instance, uint8_t addr, uint8_t *data, uint16_t len);
void      hal_i2c_register_dma_callback(HalI2cInstance instance, HalI2cDmaCallback cb);
bool      hal_i2c_is_busy(HalI2cInstance instance);

#endif /* HAL_I2C_H */
