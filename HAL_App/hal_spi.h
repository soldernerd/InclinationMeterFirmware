#ifndef HAL_SPI_H
#define HAL_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    HAL_SPI_DISPLAY  = 0,
    HAL_SPI_SCL3300  = 1,   /* stub — SCL3300 not on REV B hardware */
    HAL_SPI_DAC      = 2,   /* AD9833 waveform generator (WP7), SPI3, write-only (no MISO) */
    HAL_SPI_ADC      = 3,   /* ADS131M04 4-ch ADC (WP8), SPI1, true full-duplex (DIN + DOUT) */
    HAL_SPI_COUNT,
} HalSpiInstance;

typedef void (*HalSpiDmaCallback)(HalSpiInstance instance, bool success);

void hal_spi_init(HalSpiInstance instance);

/* Blocking write. DRV_OK on success, DRV_ERR_COMM on a HAL failure,
 * DRV_ERR_INVALID for a bad instance/args. (The display path ignores the
 * return; the AD9833 driver checks it — CLAUDE.md 7.6.) */

/* Full hardware re-init (HAL_SPI_DeInit + MX_SPIx_Init) for recovery after
 * a wedged peripheral (e.g. SPI_SR_BSY stuck set past drv_sharp_lcd's
 * drain timeout) — a software flag reset alone doesn't clear that, only
 * actually cycling the peripheral (and its clock, via Msp Deinit/Init)
 * does. Safe to call any time the instance is idle. */
void hal_spi_reinit(HalSpiInstance instance);
DrvStatus hal_spi_write(HalSpiInstance instance, const uint8_t *data, uint16_t len);
void hal_spi_write_dma(HalSpiInstance instance, const uint8_t *data, uint16_t len);

/* Blocking full-duplex transfer — HAL_SPI_ADC only, init/diagnostic use
 * (register read-back). Same data as hal_spi_transmit_receive_dma() but
 * synchronous; do not call once the streaming trigger is armed. */
DrvStatus hal_spi_transmit_receive(HalSpiInstance instance,
                                   const uint8_t *tx_data, uint8_t *rx_data,
                                   uint16_t len);

/* Full-duplex DMA transfer — HAL_SPI_ADC only. tx_data/rx_data must each
 * be >= len bytes and stay valid until the registered HalSpiDmaCallback
 * fires. Unlike hal_spi_write_dma() (transmit-only), this both sends and
 * captures the response — the ADS131M04 always shifts the previous
 * frame's response out on DOUT while a new frame is clocked in on DIN. */
DrvStatus hal_spi_transmit_receive_dma(HalSpiInstance instance,
                                       const uint8_t *tx_data, uint8_t *rx_data,
                                       uint16_t len);

void hal_spi_register_dma_callback(HalSpiInstance instance, HalSpiDmaCallback cb);
void hal_spi_cs_assert(HalSpiInstance instance);
void hal_spi_cs_deassert(HalSpiInstance instance);
bool hal_spi_is_busy(HalSpiInstance instance);

/* --- ADS131M04 raw-DMA streaming path (HAL_SPI_ADC / SPI1 only) ---
 * The HAL's HAL_SPI_TransmitReceive_DMA + SPI_EndRxTxTransaction wrapper
 * costs ~25-40 us of CPU per 18-byte frame (FIFO spin in the DMA-complete
 * ISR, state-machine churn) — too much at the ~20 kHz ADC frame rate, it
 * starves the scheduler and makes the poll/DRDY timing non-uniform. This
 * path talks to DMA1_Channel2 (SPI1_TX) / DMA1_Channel3 (SPI1_RX) and the
 * SPI1 registers directly, no HAL SPI state machine, no completion IRQ:
 *   _stream_init()   once, after MX_SPI1_Init + any blocking register I/O:
 *                    parks CPAR, arms SPI1 CR2 TX/RXDMAEN, enables SPE.
 *   _stream_begin()  kick one full-duplex frame (tx/rx must stay valid
 *                    until _stream_done()).
 *   _stream_done()   true once the RX DMA has landed every byte.
 *   _stream_end()    disable both channels, clear flags — call before the
 *                    next _stream_begin().
 * Not interrupt-model: the caller polls _stream_done() from its own timer
 * ISR. */
void hal_spi_adc_stream_init(void);
void hal_spi_adc_stream_begin(const uint8_t *tx, uint8_t *rx, uint16_t len);
bool hal_spi_adc_stream_done(void);
void hal_spi_adc_stream_end(void);

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
