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

/* Full hardware re-init (HAL_SPI_DeInit + MX_SPIx_Init) for recovery after
 * a wedged peripheral (e.g. SPI_SR_BSY stuck set past drv_sharp_lcd's
 * drain timeout) — a software flag reset alone doesn't clear that, only
 * actually cycling the peripheral (and its clock, via Msp Deinit/Init)
 * does. Safe to call any time the instance is idle. */
void hal_spi_reinit(HalSpiInstance instance);
void hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_write_dma(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb);
void hal_spi_cs_assert(HalSpiInstance instance);
void hal_spi_cs_deassert(HalSpiInstance instance);
bool hal_spi_is_busy(HalSpiInstance instance);

/* Non-blocking, single-shot check: true once the SPI peripheral has
 * finished shifting out every byte (TX FIFO empty + not BSY). The
 * DMA-complete IRQ fires when the FIFO is *fed*, not when the wire is
 * idle, so CS must not be released until this reports true or the final
 * bytes are truncated. Never spin-wait on this from ISR context — poll it
 * from a scheduler-pumped update function instead (see
 * drv_sharp_lcd_update()), since this bit is not guaranteed to clear
 * promptly and DMA1_Channel1_IRQn runs at the highest NVIC priority in
 * this system (above SysTick), so blocking here in that ISR previously
 * froze the whole MCU rather than just the display. */
bool hal_spi_tx_idle(HalSpiInstance instance);

#endif /* HAL_SPI_H */
