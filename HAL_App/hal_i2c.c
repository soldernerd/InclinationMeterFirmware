#include "hal_i2c.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern I2C_HandleTypeDef hi2c1;

/* Bounds each individual blocking HAL_I2C_Master_Transmit/Receive call
 * (hal_i2c_write/read/write_read below -- the *_dma functions don't use
 * this at all). drv_24lc256.c's EEPROM transfers are all DMA-driven and
 * never call these blocking variants; drv_bme280.c (WP9) is currently
 * their only caller, and its own per-cycle budget assumes each blocking
 * call is bounded to a few ms, not 100 -- a bus fault (device
 * unresponsive/disconnected) previously let a single blocking call
 * stall the whole cooperative scheduler for up to 100 ms, and
 * drv_bme280_update() makes several such calls per cycle. 10 ms is
 * still generous margin over a real few-byte I2C1 transaction at this
 * bus's configured speed (well under 1 ms typically). */
#define I2C_TIMEOUT_MS  10U

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

/* Short busy-wait for the bit-banged bus clear: ~5 us at 64 MHz (about 4 CPU
 * cycles per iteration), i.e. a ~100 kHz clock. Only used on the stuck-bus path. */
static void bus_clear_delay(void)
{
    for (volatile uint32_t i = 0; i < 80U; ++i) { }
}

/* I2C bus clear (NXP UM10204 3.1.16). A reset in the middle of a transaction (a debugger
 * reset, the watchdog, a brown-out) can leave a slave -- the 24LC256 or the BME280 on this
 * shared bus -- holding SDA low half-way through a byte, and it stays there until the master
 * clocks the byte out. Without this the next boot cannot talk to the EEPROM, runs on default
 * settings and calibrations, and (before svc_storage was changed to refuse that) could write
 * the defaults over the real ones. A healthy bus (both lines high) is left completely alone. */
static void bus_clear(void)
{
    if ((I2C1_SCL_PORT->IDR & I2C1_SCL_PIN) != 0U && (I2C1_SDA_PORT->IDR & I2C1_SDA_PIN) != 0U) {
        return;
    }

    (void)HAL_I2C_DeInit(&hi2c1);   /* releases both pins; MX_I2C1_Init's configuration is restored below */

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = I2C1_SCL_PIN;
    HAL_GPIO_Init(I2C1_SCL_PORT, &gpio);
    gpio.Pin   = I2C1_SDA_PIN;
    HAL_GPIO_Init(I2C1_SDA_PORT, &gpio);
    HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_SET);
    bus_clear_delay();

    /* Up to nine clocks: the slave releases SDA once the byte it was sending is complete. */
    for (uint8_t i = 0; i < 9U && HAL_GPIO_ReadPin(I2C1_SDA_PORT, I2C1_SDA_PIN) == GPIO_PIN_RESET; ++i) {
        HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_RESET);
        bus_clear_delay();
        HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_SET);
        bus_clear_delay();
    }

    /* STOP condition: SDA rises while SCL is high, so every slave goes back to idle. */
    HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_RESET);
    bus_clear_delay();
    HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_SET);
    bus_clear_delay();
    HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_SET);
    bus_clear_delay();

    HAL_GPIO_DeInit(I2C1_SCL_PORT, I2C1_SCL_PIN);
    HAL_GPIO_DeInit(I2C1_SDA_PORT, I2C1_SDA_PIN);
    (void)HAL_I2C_Init(&hi2c1);     /* MspInit puts the pins back on AF6 and re-arms the DMA channels */
}

void hal_i2c_init(HalI2cInstance instance)
{
    /* hi2c1 is initialised by MX_I2C1_Init (CubeMX). HAL_I2C_AUX is a WP2 stub. */
    if (instance == HAL_I2C_MAIN) {
        bus_clear();
    }
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

void hal_i2c_abort(HalI2cInstance instance, uint8_t addr)
{
    I2C_HandleTypeDef *h = handle_for(instance);
    if (h == 0) return;
    (void)HAL_I2C_Master_Abort_IT(h, (uint16_t)(addr << 1));
    s_busy[instance] = false;
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
