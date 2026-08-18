#ifndef HAL_SPI_H
#define HAL_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    HAL_SPI_DISPLAY  = 0,
    HAL_SPI_SCL3300  = 1,   /* stub — SCL3300 removed from REV B hardware,
                              * see Config/pin_config.h */
    HAL_SPI_DAC      = 2,   /* AD9833 waveform generator (WP7), SPI3,
                              * write-only (no MISO wired) */
    HAL_SPI_ADC      = 3,   /* ADS131M04 4-ch ADC (WP8), SPI1, true
                              * full-duplex (DIN and DOUT both used) */
    HAL_SPI_COUNT,
} HalSpiInstance;

typedef void (*HalSpiDmaCallback)(HalSpiInstance instance, bool success);

void hal_spi_init(HalSpiInstance instance);
DrvStatus hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_write_dma(HalSpiInstance instance, const uint8_t *data, uint16_t len);

/* Full-duplex DMA transfer — HAL_SPI_ADC only. tx_data/rx_data must each
 * be at least len bytes and stay valid until the registered
 * HalSpiDmaCallback fires. Unlike hal_spi_write_dma() (transmit-only,
 * used by the write-only DISPLAY/DAC instances), this both sends and
 * captures the response in one transfer — the ADS131M04 always shifts
 * out the previous frame's response on DOUT while a new command/dummy
 * frame is clocked in on DIN. */
DrvStatus hal_spi_transmit_receive_dma(HalSpiInstance instance,
                                        const uint8_t *tx_data, uint8_t *rx_data,
                                        uint16_t len);

void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb);
void hal_spi_cs_assert(HalSpiInstance instance);
void hal_spi_cs_deassert(HalSpiInstance instance);
bool hal_spi_is_busy(HalSpiInstance instance);

#endif /* HAL_SPI_H */
