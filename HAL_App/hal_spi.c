#include "hal_spi.h"
#include "hal_gpio.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;
extern SPI_HandleTypeDef hspi3;

static volatile bool      s_busy[HAL_SPI_COUNT] = { 0 };
static HalSpiDmaCallback  s_cb[HAL_SPI_COUNT]   = { 0 };

void hal_spi_init(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        /* hspi2 already initialised by MX_SPI2_Init(). Display moved from
         * SPI1 to SPI2 in the REV B pinout (SPI1 is now the external ADC
         * front-end bus). SPI2's NSS is SPI_NSS_SOFT (see
         * hal_spi_cs_assert/deassert below): the display's active-HIGH CS
         * can't use this SPI IP's hardware NSS, fixed active-low with no
         * polarity-invert bit. */
        s_busy[HAL_SPI_DISPLAY] = false;
        s_cb[HAL_SPI_DISPLAY]   = 0;
    } else if (instance == HAL_SPI_DAC) {
        /* hspi3 already initialised by MX_SPI3_Init() — CubeMX must
         * configure it 16-bit data size, CLKPolarity HIGH / CLKPhase
         * 1EDGE (SPI Mode 2 — the AD9833 captures SDATA on SCLK's
         * FALLING edge, datasheet "Serial Interface"), NSS soft (FSYNC
         * is a plain GPIO chip-select, driven by
         * hal_spi_cs_assert/deassert below, same reasoning as the
         * display's DISP_CS above). See pin_config.h's AD9833_* comments. */
        s_busy[HAL_SPI_DAC] = false;
        s_cb[HAL_SPI_DAC]   = 0;
    } else if (instance == HAL_SPI_ADC) {
        /* hspi1 already initialised by MX_SPI1_Init() — CubeMX must
         * configure it 8-bit data size, CLKPolarity LOW / CLKPhase
         * 2EDGE (SPI Mode 1 — the ADS131M04 datasheet's own "Interface"
         * section states "CPOL = 0 and CPHA = 1"), full duplex 2-lines,
         * NSS soft (ADC_CS is a plain GPIO chip-select, same reasoning
         * as DISPLAY/DAC above — held low across the whole multi-word
         * frame). Needs both RX and TX DMA channels, unlike
         * DISPLAY/DAC's TX-only DMA. See pin_config.h's ADC_* comments. */
        s_busy[HAL_SPI_ADC] = false;
        s_cb[HAL_SPI_ADC]   = 0;
    }
    /* HAL_SPI_SCL3300 — stub, sensor removed from REV B hardware */
}

DrvStatus hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len)
{
    if (data == 0 || len == 0) {
        return DRV_ERR_INVALID;
    }
    if (instance == HAL_SPI_DISPLAY) {
        s_busy[HAL_SPI_DISPLAY] = true;
        HAL_StatusTypeDef rc = HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, HAL_MAX_DELAY);
        s_busy[HAL_SPI_DISPLAY] = false;
        return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
    } else if (instance == HAL_SPI_DAC) {
        s_busy[HAL_SPI_DAC] = true;
        HAL_StatusTypeDef rc = HAL_SPI_Transmit(&hspi3, (uint8_t *)data, len, HAL_MAX_DELAY);
        s_busy[HAL_SPI_DAC] = false;
        return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
    } else if (instance == HAL_SPI_ADC) {
        /* Blocking TX used only by drv_ads131m04.c's one-time register
         * writes during init -- streaming reads use
         * hal_spi_transmit_receive_dma() below instead. HAL_SPI_Transmit()
         * on a full-duplex peripheral still clocks MISO; the caller just
         * doesn't capture it, same as the DISPLAY/DAC TX-only instances
         * above ignoring whatever (if anything) comes back on their MISO. */
        s_busy[HAL_SPI_ADC] = true;
        HAL_StatusTypeDef rc = HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, HAL_MAX_DELAY);
        s_busy[HAL_SPI_ADC] = false;
        return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
    }
    return DRV_ERR_INVALID;
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

DrvStatus hal_spi_transmit_receive_dma(HalSpiInstance instance,
                                        const uint8_t *tx_data, uint8_t *rx_data,
                                        uint16_t len)
{
    if (instance != HAL_SPI_ADC || tx_data == 0 || rx_data == 0 || len == 0) {
        return DRV_ERR_INVALID;
    }
    if (s_busy[HAL_SPI_ADC]) {
        return DRV_ERR_NOT_READY;
    }
    s_busy[HAL_SPI_ADC] = true;
    if (HAL_SPI_TransmitReceive_DMA(&hspi1, (uint8_t *)tx_data, rx_data, len) != HAL_OK) {
        s_busy[HAL_SPI_ADC] = false;
        return DRV_ERR_COMM;
    }
    return DRV_OK;
}

void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb)
{
    if (instance < HAL_SPI_COUNT) {
        s_cb[instance] = cb;
    }
}

void hal_spi_cs_assert(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        /* Sharp LCD: CS active HIGH */
        hal_gpio_set(DISP_CS_PORT, DISP_CS_PIN, true);
    } else if (instance == HAL_SPI_DAC) {
        /* AD9833 FSYNC: active LOW */
        hal_gpio_set(AD9833_FSYNC_PORT, AD9833_FSYNC_PIN, false);
    } else if (instance == HAL_SPI_ADC) {
        /* ADS131M04 CS: active LOW */
        hal_gpio_set(ADC_CS_PORT, ADC_CS_PIN, false);
    }
}

void hal_spi_cs_deassert(HalSpiInstance instance)
{
    if (instance == HAL_SPI_DISPLAY) {
        hal_gpio_set(DISP_CS_PORT, DISP_CS_PIN, false);
    } else if (instance == HAL_SPI_DAC) {
        hal_gpio_set(AD9833_FSYNC_PORT, AD9833_FSYNC_PIN, true);
    } else if (instance == HAL_SPI_ADC) {
        hal_gpio_set(ADC_CS_PORT, ADC_CS_PIN, true);
    }
}

bool hal_spi_is_busy(HalSpiInstance instance)
{
    if (instance < HAL_SPI_COUNT) {
        return s_busy[instance];
    }
    return false;
}

/* HAL weak override — fires when a TX-only DMA transfer completes
 * (HAL_SPI_Transmit_DMA — DISPLAY only; DAC never uses DMA). */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2) {
        s_busy[HAL_SPI_DISPLAY] = false;
        if (s_cb[HAL_SPI_DISPLAY]) {
            s_cb[HAL_SPI_DISPLAY](HAL_SPI_DISPLAY, true);
        }
    }
}

/* HAL weak override — fires when a full-duplex DMA transfer completes
 * (HAL_SPI_TransmitReceive_DMA — ADC only, a separate weak function from
 * HAL_SPI_TxCpltCallback above). */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        s_busy[HAL_SPI_ADC] = false;
        if (s_cb[HAL_SPI_ADC]) {
            s_cb[HAL_SPI_ADC](HAL_SPI_ADC, true);
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
    } else if (hspi->Instance == SPI1) {
        s_busy[HAL_SPI_ADC] = false;
        if (s_cb[HAL_SPI_ADC]) {
            s_cb[HAL_SPI_ADC](HAL_SPI_ADC, false);
        }
    }
}
