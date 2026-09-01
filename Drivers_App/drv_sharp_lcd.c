#include "drv_sharp_lcd.h"
#include "hal_spi.h"
#include "hal_gpio.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include "pin_config.h"
#include <string.h>

#define CMD_WRITE_LINE   0x80U
#define TRAILER          0x00U
/* line addr (1) + 50 data + per-line trailing dummy byte (1) = 52.
 * The LS027B7DH01 multi-line write needs 8 dummy clocks after every line's
 * data, not just at the end (see Adafruit_SharpMem / u8g2 ls027 driver). */
#define ROW_PACKET_SIZE  (1U + LCD_STRIDE + 1U)
#define TRAILER_BYTES    8U                  /* >> datasheet's single trailing period */

/* Generous SCS timing margins. Datasheet asks for t_sSCS >= 3 us setup and
 * t_hSCS >= 1 us hold; anything under ~10 ms is immaterial to the update
 * rate here, so use milliseconds and stop worrying about it. */
#define SCS_SETUP_US     5000U
#define SCS_HOLD_US      5000U
#define DISP_ON_SETTLE_US 10000U

/* One contiguous DMA buffer:
 *   [0x80] [line(1) | 50B data | 0x00] ... [line(240) | 50B data | 0x00] [8x 0x00]
 * Total = 1 + 240*52 + 8 = 12489 bytes.
 */
#define TX_BUFFER_SIZE  (1U + LCD_HEIGHT * ROW_PACKET_SIZE + TRAILER_BYTES)
static uint8_t s_tx_buf[TX_BUFFER_SIZE];

static volatile bool s_busy = false;

static inline uint8_t *row_pixels(uint16_t row)
{
    return &s_tx_buf[1U + row * ROW_PACKET_SIZE + 1U];
}

/* Sharp LS027B7DH01 expects LSB-first on the wire for command and address
 * bytes. With MSB-first SPI we bit-reverse cmd/address so wire order matches.
 * Pixel data is already in bit-7-leftmost format and goes through unchanged. */
static inline uint8_t bit_reverse(uint8_t b)
{
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xCCU) >> 2) | ((b & 0x33U) << 2));
    b = (uint8_t)(((b & 0xAAU) >> 1) | ((b & 0x55U) << 1));
    return b;
}

static void on_dma_complete(HalSpiInstance instance, bool success)
{
    (void)success;
    if (instance == HAL_SPI_DISPLAY) {
        /* The DMA-complete IRQ fires when the FIFO is fed, not when the
         * last bit has left the pin. Wait for the peripheral to go idle,
         * then hold CS a generous margin before releasing it, or the
         * final line and trailer get chopped. */
        hal_spi_wait_tx_done(HAL_SPI_DISPLAY);
        hal_systick_delay_us(SCS_HOLD_US);
        hal_spi_cs_deassert(HAL_SPI_DISPLAY);
        s_busy = false;
    }
}

static void prime_tx_buffer(void)
{
    /* CMD_WRITE_LINE 0x80 is already bit-reverse of 0x01 — send as-is */
    s_tx_buf[0] = CMD_WRITE_LINE;
    for (uint16_t r = 0; r < LCD_HEIGHT; ++r) {
        s_tx_buf[1U + r * ROW_PACKET_SIZE] = bit_reverse((uint8_t)(r + 1U));
        /* per-line trailing dummy byte, after the 50 data bytes */
        s_tx_buf[1U + r * ROW_PACKET_SIZE + 1U + LCD_STRIDE] = TRAILER;
    }
    memset(&s_tx_buf[1U + LCD_HEIGHT * ROW_PACKET_SIZE], TRAILER, TRAILER_BYTES);
    drv_sharp_lcd_clear_buffer();
}

void drv_sharp_lcd_init(void)
{
    s_busy = false;
    prime_tx_buffer();

    hal_spi_init(HAL_SPI_DISPLAY);
    hal_spi_register_dma_callback(HAL_SPI_DISPLAY, on_dma_complete);

    /* PD3 VCOM square wave (5 Hz, within the datasheet's 1-10 Hz window) —
     * no hardware PWM channel on REV B, toggled manually in the TIM6
     * period-elapsed ISR (hal_tim.c). */
    hal_tim_vcom_start();

    /* Power up the display, then let its internal supply settle before the
     * first write (generous margin over the datasheet's power-up time). */
    hal_gpio_set(DISP_ON_PORT, DISP_ON_PIN, true);
    hal_systick_delay_us(DISP_ON_SETTLE_US);

    drv_sharp_lcd_clear_display();
}

void drv_sharp_lcd_clear_buffer(void)
{
    for (uint16_t r = 0; r < LCD_HEIGHT; ++r) {
        memset(row_pixels(r), 0x00, LCD_STRIDE);
    }
}

void drv_sharp_lcd_selftest_fill(void)
{
    for (uint16_t r = 0; r < LCD_HEIGHT; ++r) {
        uint8_t  fill = (r < 8U) ? 0xFFU
                                 : (((r >> 4) & 1U) ? 0xFFU : 0x00U);
        uint8_t *px = row_pixels(r);
        memset(px, fill, LCD_STRIDE);
        px[0] = 0xFFU;   /* left-edge vertical bar */
    }
}

void drv_sharp_lcd_clear_display(void)
{
    drv_sharp_lcd_clear_buffer();
    (void)drv_sharp_lcd_flush_full();
}

void drv_sharp_lcd_set_pixel(uint16_t x, uint16_t y, bool on)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    uint8_t *row = row_pixels(y);
    uint16_t byte_idx = x >> 3;
    uint8_t  mask     = (uint8_t)(0x80U >> (x & 7U));   /* MSB-first within byte */
    if (on) row[byte_idx] |= mask;
    else    row[byte_idx] &= (uint8_t)~mask;
}

void drv_sharp_lcd_write_row(uint16_t row, const uint8_t *src)
{
    if (row >= LCD_HEIGHT || src == 0) return;
    memcpy(row_pixels(row), src, LCD_STRIDE);
}

void drv_sharp_lcd_mark_dirty(uint16_t row)  { (void)row; /* WP1: full-flush only */ }
void drv_sharp_lcd_mark_all_dirty(void)       { /* WP1: full-flush only */ }

DrvStatus drv_sharp_lcd_flush(void)
{
    return drv_sharp_lcd_flush_full();
}

DrvStatus drv_sharp_lcd_flush_full(void)
{
    if (s_busy) return DRV_ERR_NOT_READY;
    s_busy = true;
    hal_spi_cs_assert(HAL_SPI_DISPLAY);
    /* SCS setup time before the first clock -- generous margin. */
    hal_systick_delay_us(SCS_SETUP_US);
    hal_spi_write_dma(HAL_SPI_DISPLAY, s_tx_buf, (uint16_t)TX_BUFFER_SIZE);
    return DRV_OK;
}

bool drv_sharp_lcd_is_busy(void)
{
    return s_busy;
}
