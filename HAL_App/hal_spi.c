#include "hal_spi.h"
#include "hal_gpio.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern SPI_HandleTypeDef hspi2;

static volatile bool      s_busy[2]   = { false, false };
static HalSpiDmaCallback  s_cb[2]     = { 0, 0 };

void hal_spi_init(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        /* hspi2 already initialised by MX_SPI2_Init() */
        s_busy[HAL_SPI_DISPLAY] = false;
        s_cb[HAL_SPI_DISPLAY]   = 0;
    }
    /* HAL_SPI_SCL3300 — WP1 stub, implemented in WPx */
}

void hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len)
{
    if (instance != HAL_SPI_DISPLAY || data == 0 || len == 0) {
        return;
    }
    s_busy[HAL_SPI_DISPLAY] = true;
    HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, HAL_MAX_DELAY);
    s_busy[HAL_SPI_DISPLAY] = false;
}

void hal_spi_write_dma(HalSpiInstance instance, const uint8_t *data, uint16_t len)
{
    if (instance != HAL_SPI_DISPLAY || data == 0 || len == 0) {
        return;
    }
    s_busy[HAL_SPI_DISPLAY] = true;
    if (HAL_SPI_Transmit_DMA(&hspi2, (uint8_t *)data, len) != HAL_OK) {
        s_busy[HAL_SPI_DISPLAY] = false;
        if (s_cb[HAL_SPI_DISPLAY]) {
            s_cb[HAL_SPI_DISPLAY](HAL_SPI_DISPLAY, false);
        }
    }
}

void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb)
{
    if (instance < 2U) {
        s_cb[instance] = cb;
    }
}

void hal_spi_cs_assert(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        /* Sharp LCD: CS active HIGH */
        hal_gpio_set(DISP_CS_PORT, DISP_CS_PIN, true);
    }
}

void hal_spi_cs_deassert(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        hal_gpio_set(DISP_CS_PORT, DISP_CS_PIN, false);
    }
}

bool hal_spi_is_busy(HalSpiInstance instance)
{
    if (instance < 2U) {
        return s_busy[instance];
    }
    return false;
}

void hal_spi_wait_tx_done(HalSpiInstance instance)
{
    if (instance != HAL_SPI_DISPLAY) {
        return;
    }
    /* Drain the TX FIFO, then wait for the shift register to empty. */
    while ((hspi2.Instance->SR & SPI_SR_FTLVL) != 0U) { }
    while ((hspi2.Instance->SR & SPI_SR_BSY)   != 0U) { }
}

/* HAL weak override — fires when DMA TX completes */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2) {
        s_busy[HAL_SPI_DISPLAY] = false;
        if (s_cb[HAL_SPI_DISPLAY]) {
            s_cb[HAL_SPI_DISPLAY](HAL_SPI_DISPLAY, true);
        }
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2) {
        s_busy[HAL_SPI_DISPLAY] = false;
        if (s_cb[HAL_SPI_DISPLAY]) {
            s_cb[HAL_SPI_DISPLAY](HAL_SPI_DISPLAY, false);
        }
    }
}
