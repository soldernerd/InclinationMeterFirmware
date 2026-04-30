#include "u8g2_hal_callback.h"
#include "drv_sharp_lcd.h"
#include "stm32g0xx_hal.h"

/* u8g2's u8x8_d_ls027b7dh01_400x240 driver emits, per DRAW_TILE call:
 *   [cmd=0x80] [line+50B+0x00] x 8 [final 0x00]
 * That's 1 + 8*52 + 1 = 418 bytes per transfer; 30 transfers per frame.
 * We parse the stream, write pixel rows to drv_sharp_lcd's framebuffer,
 * and let app_display_update() trigger one DMA flush after u8g2_SendBuffer().
 */

static inline uint8_t bit_reverse(uint8_t b)
{
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xCCU) >> 2) | ((b & 0x33U) << 2));
    b = (uint8_t)(((b & 0xAAU) >> 1) | ((b & 0x55U) << 1));
    return b;
}

#define DATA_BYTES_PER_LINE   50
#define LINES_PER_TRANSFER     8
#define BYTES_PER_LINE_GROUP  (1 + DATA_BYTES_PER_LINE + 1)

static uint16_t s_byte_pos    = 0;
static uint16_t s_current_row = 0;
static uint8_t  s_row_buf[DATA_BYTES_PER_LINE];
static uint8_t  s_row_pos     = 0;

static void parse_byte(uint8_t b)
{
    /* Position 0: cmd byte (0x80) — ignore */
    if (s_byte_pos == 0) {
        s_byte_pos++;
        return;
    }

    /* Positions 1 .. 1 + 8*52 - 1 = 1..416 : 8 line groups */
    if (s_byte_pos <= LINES_PER_TRANSFER * BYTES_PER_LINE_GROUP) {
        uint16_t group_pos = (uint16_t)(s_byte_pos - 1);
        uint16_t in_group  = group_pos % BYTES_PER_LINE_GROUP;

        if (in_group == 0) {
            /* Line address (SWAP8'd 1-based line number) */
            s_current_row = (uint16_t)(bit_reverse(b) - 1U);
            s_row_pos = 0;
        } else if (in_group <= DATA_BYTES_PER_LINE) {
            /* Pixel byte */
            if (s_row_pos < DATA_BYTES_PER_LINE) {
                s_row_buf[s_row_pos++] = b;
            }
            if (in_group == DATA_BYTES_PER_LINE) {
                drv_sharp_lcd_write_row(s_current_row, s_row_buf);
            }
        }
        /* in_group == DATA_BYTES_PER_LINE + 1: trailer 0x00 — ignore */

        s_byte_pos++;
        return;
    }

    /* Final trailer byte — ignore */
    s_byte_pos++;
}

uint8_t u8g2_hal_callback(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    switch (msg) {
        case U8X8_MSG_BYTE_INIT:
            /* SPI is already initialised by hal_spi_init(); nothing to do */
            return 1;

        case U8X8_MSG_BYTE_SET_DC:
            /* Sharp LCD has no D/C pin */
            return 1;

        case U8X8_MSG_BYTE_START_TRANSFER:
            s_byte_pos = 0;
            s_row_pos  = 0;
            return 1;

        case U8X8_MSG_BYTE_SEND: {
            const uint8_t *p = (const uint8_t *)arg_ptr;
            for (uint8_t i = 0; i < arg_int; ++i) {
                parse_byte(p[i]);
            }
            return 1;
        }

        case U8X8_MSG_BYTE_END_TRANSFER:
            /* Frame flush is driven by app_display_update(), not per-tile */
            return 1;

        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            return 1;

        case U8X8_MSG_DELAY_MILLI:
            HAL_Delay(arg_int);
            return 1;

        case U8X8_MSG_DELAY_NANO:
        case U8X8_MSG_DELAY_10MICRO:
        case U8X8_MSG_DELAY_100NANO:
            return 1;

        case U8X8_MSG_GPIO_CS:
        case U8X8_MSG_GPIO_DC:
        case U8X8_MSG_GPIO_RESET:
            return 1;

        default:
            return 0;
    }
}

void u8g2_hal_init(u8g2_t *u8g2)
{
    u8g2_Setup_ls027b7dh01_400x240_f(
        u8g2,
        U8G2_R0,
        u8g2_hal_callback,    /* byte_cb */
        u8g2_hal_callback);   /* gpio_and_delay_cb */
}
