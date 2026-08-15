#include "hal_i2c.h"
#include "stm32g0xx_hal.h"

extern I2C_HandleTypeDef hi2c1;

#define I2C_TIMEOUT_MS  100U

static volatile bool      s_busy[HAL_I2C_COUNT] = { false, false };
static HalI2cDmaCallback  s_cb[HAL_I2C_COUNT]   = { 0, 0 };

static I2C_HandleTypeDef *handle_for(HalI2cInstance instance)
{
    switch (instance) {
        case HAL_I2C_MAIN: return &hi2c1;
        case HAL_I2C_AUX:  return 0;            /* WP2 stub */
        default:           return 0;
    }
}

void hal_i2c_init(HalI2cInstance instance)
{
    /* hi2c1 is initialised by MX_I2C1_Init (CubeMX). Nothing else to do
     * for HAL_I2C_MAIN. HAL_I2C_AUX is a WP2 stub. */
    if (instance < HAL_I2C_COUNT) {
        s_busy[instance] = false;
        s_cb[instance]   = 0;
    }
}

DrvStatus hal_i2c_write(HalI2cInstance instance, uint8_t addr,
                        const uint8_t *data, uint16_t len)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0 || data == 0) return DRV_ERR_INVALID;
    /* I2C addresses passed as 7-bit → HAL expects them shifted left by 1 */
    return (HAL_I2C_Master_Transmit(h, (uint16_t)(addr << 1), (uint8_t *)data,
                                    len, I2C_TIMEOUT_MS) == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
}

DrvStatus hal_i2c_read(HalI2cInstance instance, uint8_t addr,
                       uint8_t *data, uint16_t len)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0 || data == 0) return DRV_ERR_INVALID;
    return (HAL_I2C_Master_Receive(h, (uint16_t)(addr << 1), data,
                                   len, I2C_TIMEOUT_MS) == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
}

DrvStatus hal_i2c_write_read(HalI2cInstance instance, uint8_t addr,
                             const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_len)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0 || tx == 0 || rx == 0) return DRV_ERR_INVALID;
    if (HAL_I2C_Master_Transmit(h, (uint16_t)(addr << 1), (uint8_t *)tx,
                                tx_len, I2C_TIMEOUT_MS) != HAL_OK) {
        return DRV_ERR_COMM;
    }
    return (HAL_I2C_Master_Receive(h, (uint16_t)(addr << 1), rx,
                                   rx_len, I2C_TIMEOUT_MS) == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
}

void hal_i2c_read_dma(HalI2cInstance instance, uint8_t addr,
                      uint8_t *buf, uint16_t len)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0 || buf == 0) return;
    s_busy[instance] = true;
    if (HAL_I2C_Master_Receive_DMA(h, (uint16_t)(addr << 1), buf, len) != HAL_OK) {
        s_busy[instance] = false;
        if (s_cb[instance]) s_cb[instance](instance, false);
    }
}

void hal_i2c_write_dma(HalI2cInstance instance, uint8_t addr,
                       const uint8_t *buf, uint16_t len)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0 || buf == 0) return;
    s_busy[instance] = true;
    if (HAL_I2C_Master_Transmit_DMA(h, (uint16_t)(addr << 1), (uint8_t *)buf, len) != HAL_OK) {
        s_busy[instance] = false;
        if (s_cb[instance]) s_cb[instance](instance, false);
    }
}

void hal_i2c_register_dma_callback(HalI2cInstance instance, HalI2cDmaCallback cb)
{
    if (instance < HAL_I2C_COUNT) {
        s_cb[instance] = cb;
    }
}

bool hal_i2c_is_busy(HalI2cInstance instance)
{
    if (instance < HAL_I2C_COUNT) {
        return s_busy[instance];
    }
    return false;
}

bool hal_i2c_device_ready(HalI2cInstance instance, uint8_t addr)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0) return false;
    /* Poll one trial with a short timeout — used to detect EEPROM ACK
     * after a write cycle completes. */
    return HAL_I2C_IsDeviceReady(h, (uint16_t)(addr << 1), 1, 2) == HAL_OK;
}

/* HAL weak overrides — fire on DMA complete */
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == hi2c1.Instance) {
        s_busy[HAL_I2C_MAIN] = false;
        if (s_cb[HAL_I2C_MAIN]) s_cb[HAL_I2C_MAIN](HAL_I2C_MAIN, true);
    }
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == hi2c1.Instance) {
        s_busy[HAL_I2C_MAIN] = false;
        if (s_cb[HAL_I2C_MAIN]) s_cb[HAL_I2C_MAIN](HAL_I2C_MAIN, true);
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == hi2c1.Instance) {
        s_busy[HAL_I2C_MAIN] = false;
        if (s_cb[HAL_I2C_MAIN]) s_cb[HAL_I2C_MAIN](HAL_I2C_MAIN, false);
    }
}
