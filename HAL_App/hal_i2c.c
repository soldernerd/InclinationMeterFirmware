/* WP1 stub — implemented in WPx */
#include "hal_i2c.h"

void      hal_i2c_init(HalI2cInstance instance)                                       { (void)instance; }
DrvStatus hal_i2c_write(HalI2cInstance instance, uint8_t addr, const uint8_t *data, uint16_t len) { (void)instance; (void)addr; (void)data; (void)len; return DRV_OK; }
DrvStatus hal_i2c_read(HalI2cInstance instance, uint8_t addr, uint8_t *data, uint16_t len)        { (void)instance; (void)addr; (void)data; (void)len; return DRV_OK; }
void      hal_i2c_register_dma_callback(HalI2cInstance instance, HalI2cDmaCallback cb)            { (void)instance; (void)cb; }
bool      hal_i2c_is_busy(HalI2cInstance instance)                                                { (void)instance; return false; }
