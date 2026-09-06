#include "hal_spi.h"
#include "hal_gpio.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"
#include "spi.h"

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
        /* hspi3 already initialised by MX_SPI3_Init(): 16-bit, SPI Mode 2
         * (CLKPolarity HIGH / CLKPhase 1EDGE — the AD9833 latches SDATA on
         * SCLK's falling edge, datasheet "Serial Interface"), NSS soft
         * (FSYNC is a plain GPIO driven by hal_spi_cs_assert/deassert
         * below, active LOW — see pin_config.h's AD9833_* comments). */
        s_busy[HAL_SPI_DAC] = false;
        s_cb[HAL_SPI_DAC]   = 0;
    } else if (instance == HAL_SPI_ADC) {
        /* hspi1 already initialised by MX_SPI1_Init(): 8-bit, SPI Mode 1
         * (CLKPolarity LOW / CLKPhase 2EDGE — datasheet "CPOL = 0 and
         * CPHA = 1"), full-duplex 2-line, NSS soft (ADC_CS is a plain
         * GPIO held low across the whole multi-word frame). Uses both RX
         * and TX DMA channels, unlike the TX-only DISPLAY/DAC paths. */
        s_busy[HAL_SPI_ADC] = false;
        s_cb[HAL_SPI_ADC]   = 0;
    }
    /* HAL_SPI_SCL3300 — stub, sensor not on REV B hardware */
}

void hal_spi_reinit(HalSpiInstance instance)
{
    if (instance != HAL_SPI_DISPLAY) {
        return;
    }
    /* Full peripheral cycle (clock off/on via Msp Deinit/Init) — clears a
     * stuck SPI_SR_BSY that a software-only flag reset cannot. */
    (void)HAL_SPI_DeInit(&hspi2);
    MX_SPI2_Init();
    s_busy[HAL_SPI_DISPLAY] = false;
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
    }
    if (instance == HAL_SPI_DAC) {
        /* 16-bit data size: len is a word count, HAL reads 2 bytes/word. */
        s_busy[HAL_SPI_DAC] = true;
        HAL_StatusTypeDef rc = HAL_SPI_Transmit(&hspi3, (uint8_t *)data, len, HAL_MAX_DELAY);
        s_busy[HAL_SPI_DAC] = false;
        return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
    }
    if (instance == HAL_SPI_ADC) {
        /* Blocking TX — only drv_ads131m04.c's one-time register writes
         * at init. Streaming reads use hal_spi_transmit_receive_dma().
         * MISO is still clocked; the caller just doesn't capture it. */
        s_busy[HAL_SPI_ADC] = true;
        HAL_StatusTypeDef rc = HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, HAL_MAX_DELAY);
        s_busy[HAL_SPI_ADC] = false;
        return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
    }
    return DRV_ERR_INVALID;
}

/* ---- ADS131M04 raw-DMA streaming path (see hal_spi.h) ---- */

#define ADC_TX_DMA   DMA1_Channel2   /* SPI1_TX, per Core/Src/spi.c MspInit */
#define ADC_RX_DMA   DMA1_Channel3   /* SPI1_RX */
/* DMA1 ISR/IFCR bit positions: channel n uses bits [4n-4 .. 4n-1]. */
#define ADC_TX_DMA_GIF   DMA_IFCR_CGIF2
#define ADC_RX_DMA_GIF   DMA_IFCR_CGIF3

void hal_spi_adc_stream_init(void)
{
    /* Channels were configured by HAL_DMA_Init() in MX_SPI1_Init's MspInit
     * (request routing, byte size, MINC on memory, direction, no circular,
     * interrupts off). HAL_DMA_Init does NOT set CPAR — do it here, once. */
    ADC_TX_DMA->CCR &= ~DMA_CCR_EN;
    ADC_RX_DMA->CCR &= ~DMA_CCR_EN;
    ADC_TX_DMA->CPAR = (uint32_t)&hspi1.Instance->DR;
    ADC_RX_DMA->CPAR = (uint32_t)&hspi1.Instance->DR;
    DMA1->IFCR = ADC_TX_DMA_GIF | ADC_RX_DMA_GIF;

    /* Arm SPI1 for DMA and enable it. From here the peripheral clocks a
     * frame whenever the TX DMA channel is enabled with a byte count. */
    SET_BIT(hspi1.Instance->CR2, SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    __HAL_SPI_ENABLE(&hspi1);
    s_busy[HAL_SPI_ADC] = false;
}

void hal_spi_adc_stream_begin(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    /* RX armed before TX so it catches the very first incoming byte. */
    ADC_RX_DMA->CCR &= ~DMA_CCR_EN;
    ADC_TX_DMA->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = ADC_TX_DMA_GIF | ADC_RX_DMA_GIF;

    ADC_RX_DMA->CMAR  = (uint32_t)rx;
    ADC_RX_DMA->CNDTR = len;
    ADC_TX_DMA->CMAR  = (uint32_t)tx;
    ADC_TX_DMA->CNDTR = len;

    s_busy[HAL_SPI_ADC] = true;
    ADC_RX_DMA->CCR |= DMA_CCR_EN;
    ADC_TX_DMA->CCR |= DMA_CCR_EN;
}

bool hal_spi_adc_stream_done(void)
{
    return ADC_RX_DMA->CNDTR == 0U;
}

void hal_spi_adc_stream_end(void)
{
    ADC_TX_DMA->CCR &= ~DMA_CCR_EN;
    ADC_RX_DMA->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = ADC_TX_DMA_GIF | ADC_RX_DMA_GIF;
    s_busy[HAL_SPI_ADC] = false;
}

DrvStatus hal_spi_transmit_receive(HalSpiInstance instance,
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
    HAL_StatusTypeDef rc = HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)tx_data, rx_data,
                                                   len, HAL_MAX_DELAY);
    s_busy[HAL_SPI_ADC] = false;
    return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
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
    if (instance < (unsigned)HAL_SPI_COUNT) {
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
    if (instance < (unsigned)HAL_SPI_COUNT) {
        return s_busy[instance];
    }
    return false;
}

bool hal_spi_tx_idle(HalSpiInstance instance)
{
    if (instance != HAL_SPI_DISPLAY) {
        return true;
    }
    /* TX FIFO empty and shift register not busy — single poll, no loop. */
    return (hspi2.Instance->SR & (SPI_SR_FTLVL | SPI_SR_BSY)) == 0U;
}

/* HAL weak override — fires when a TX-only DMA transfer completes
 * (HAL_SPI_Transmit_DMA — DISPLAY only). */
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
 * (HAL_SPI_TransmitReceive_DMA — ADC only, a distinct weak function). */
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
