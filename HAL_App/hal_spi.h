#ifndef HAL_SPI_H
#define HAL_SPI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HAL_SPI_DISPLAY  = 0,
    HAL_SPI_SCL3300  = 1,
} HalSpiInstance;

typedef void (*HalSpiDmaCallback)(HalSpiInstance instance, bool success);

void hal_spi_init(HalSpiInstance instance);
void hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_write_dma(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb);
void hal_spi_cs_assert(HalSpiInstance instance);
void hal_spi_cs_deassert(HalSpiInstance instance);
bool hal_spi_is_busy(HalSpiInstance instance);

/* Block until the SPI peripheral has finished shifting out every byte
 * (TX FIFO empty + not BSY). The DMA-complete IRQ fires when the FIFO is
 * *fed*, not when the wire is idle, so CS must not be released until this
 * returns or the final bytes are truncated. */
void hal_spi_wait_tx_done(HalSpiInstance instance);

#endif /* HAL_SPI_H */
