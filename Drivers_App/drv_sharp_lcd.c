#include "drv_sharp_lcd.h"
#include "hal_spi.h"
#include "hal_gpio.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include "pin_config.h"
#include <string.h>

/* Display render path: CMakeLists.txt pins this file to -O2 in every
 * config. At -O0 a full banded render is tens of ms and the per-tick
 * cost bound (docs/display_page_render.md) no longer holds. Fail the
 * build if the pin is lost. */
#if !defined(__OPTIMIZE__)
#error "display-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

#define CMD_WRITE_LINE   0x80U
#define TRAILER          0x00U
/* line addr (1) + 50 data + per-line trailing dummy byte (1) = 52.
 * The LS027B7DH01 multi-line write needs 8 dummy clocks after every line's
 * data, not just at the end (see Adafruit_SharpMem / u8g2 ls027 driver). */
#define ROW_PACKET_SIZE  (1U + LCD_STRIDE + 1U)
#define TRAILER_BYTES    8U                  /* >> datasheet's single trailing period */

/* SCS timing margins: datasheet asks for t_sSCS >= 3 us setup and
 * t_hSCS >= 1 us hold. 2x the datasheet minimum is comfortable headroom
 * without being needlessly long — both of these run in thread context
 * (drv_sharp_lcd_flush_dirty() / drv_sharp_lcd_update(), never an ISR), but
 * task_display() pumps drv_sharp_lcd_update() from the single-threaded
 * scheduler loop, so every microsecond here delays every other task. */
#define SCS_SETUP_US     6U
#define SCS_HOLD_US      2U
#define DISP_ON_SETTLE_US 10000U

/* Give up waiting for the SPI shift register to drain after this long and
 * release CS anyway, same "don't wedge forever" policy as drv_24lc256.c's
 * write-cycle poll — see drv_sharp_lcd_update(). */
#define SPI_DRAIN_TIMEOUT_MS  50U

/* One contiguous DMA buffer:
 *   [0x80] [line(1) | 50B data | 0x00] ... [line(240) | 50B data | 0x00] [8x 0x00]
 * Total = 1 + 240*52 + 8 = 12489 bytes.
 */
#define TX_BUFFER_SIZE  (1U + LCD_HEIGHT * ROW_PACKET_SIZE + TRAILER_BYTES)
static uint8_t s_tx_buf[TX_BUFFER_SIZE];

static volatile bool s_busy = false;

/* Dirty-row tracking (partial updates). drv_sharp_lcd_write_row() compares
 * every incoming row with what is already in s_tx_buf and sets a bit only
 * when it differs, so the App layer needs no knowledge of which rows it
 * actually changed. A flush sends just those rows. The panel is a memory LCD
 * and keeps its contents, so unchanged rows never need re-sending.
 * Bits are cleared when a flush starts; they are set for the whole panel at
 * init and whenever a flush is known to have been disturbed. */
#define DIRTY_WORDS      ((LCD_HEIGHT + 31U) / 32U)
static uint32_t s_dirty[DIRTY_WORDS];

/* A flush is a list of row runs, each sent as its own CS-framed
 * transaction (cmd, rows, 8 dummy clocks). Clean gaps of up to
 * RUN_MERGE_GAP_ROWS rows are sent along with the run rather than costing a
 * transaction of their own (a row is ~208 us at 2 MHz; a transaction costs
 * about one scheduler tick). More than MAX_RUNS runs collapse to one span. */
#define MAX_RUNS            8U
#define RUN_MERGE_GAP_ROWS  8U
typedef struct { uint16_t first; uint16_t last; } RowRun;
static RowRun   s_runs[MAX_RUNS];
static uint8_t  s_run_count = 0;
static uint8_t  s_run_idx   = 0;

/* A run's DMA span is carved out of the one contiguous buffer: the byte
 * before the first row (previous row's trailing dummy, or the real command
 * byte for row 0) is temporarily set to the write-line command, and the
 * 8 bytes after the last row's dummy byte (next row's address and first
 * data bytes, or the real trailer after row 239) are temporarily zeroed.
 * Both are restored when the run has drained. Nothing touches the buffer
 * while s_busy is set (the App layer refuses to render then). */
#define RUN_TRAILER_BYTES  TRAILER_BYTES
static uint16_t s_saved_cmd_idx   = 0;
static uint8_t  s_saved_cmd       = 0;
static uint16_t s_saved_tail_idx  = 0;
static uint8_t  s_saved_tail[RUN_TRAILER_BYTES];

/* Panel health, private to this driver (thread context only — no ISR
 * touches it, unlike s_busy). false once a flush drained-timeout forced a
 * peripheral re-init; self-heals on the next clean flush. The App layer
 * mirrors this into g_system_state.display_ok via drv_sharp_lcd_ok() —
 * the driver does not reach up into system state itself. */
static bool s_ok = true;

/* True once the DMA has fed the whole frame into the SPI FIFO and we're
 * waiting for the shift register to actually drain + a CS hold margin
 * before releasing CS — pumped from drv_sharp_lcd_update(), never from
 * ISR context (see on_dma_complete()). */
static volatile bool s_awaiting_drain = false;
static uint32_t      s_drain_start_ms = 0;

/* True from drv_sharp_lcd_init() until DISP_ON_SETTLE_US has actually
 * elapsed — pumped (non-blocking) from drv_sharp_lcd_update() instead of
 * a blocking delay inside init(). s_busy stays true the whole time so any
 * flush attempted before the display is settled is correctly refused. */
static volatile bool s_pending_power_settle = false;
static uint32_t      s_settle_deadline_ms   = 0;

static inline uint8_t *row_pixels(uint16_t row)
{
    return &s_tx_buf[1U + row * ROW_PACKET_SIZE + 1U];
}

static void clear_buffer(void)
{
    for (uint16_t r = 0; r < LCD_HEIGHT; ++r) {
        memset(row_pixels(r), 0x00, LCD_STRIDE);
    }
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
        /* ISR context (DMA1_Channel1_IRQn, NVIC priority 0 — the highest
         * in this system, above SysTick's priority 3, so SysTick cannot
         * preempt it). MUST NOT block here: the DMA-complete IRQ fires
         * when the FIFO is fed, not when the wire is idle, and this used
         * to spin-wait right here for the shift register to drain. If
         * SPI_SR_BSY ever failed to clear promptly, that loop never
         * returned, SysTick froze with it, and the whole MCU hung — not
         * just the display. Just record that a drain is needed and hand
         * off to drv_sharp_lcd_update(), pumped from the scheduler. */
        s_drain_start_ms = hal_systick_get_ms();
        s_awaiting_drain = true;
    }
}

static inline bool row_is_dirty(uint16_t r)
{
    return (s_dirty[r >> 5] >> (r & 31U)) & 1U;
}

static void restore_run_bytes(void)
{
    s_tx_buf[s_saved_cmd_idx] = s_saved_cmd;
    memcpy(&s_tx_buf[s_saved_tail_idx], s_saved_tail, RUN_TRAILER_BYTES);
}

/* Start the DMA for run s_runs[i]: CS, setup time, then cmd + rows + 8
 * dummy clocks. See the comment on s_saved_cmd_idx for the buffer carving. */
static void start_run(uint8_t i)
{
    uint16_t first = s_runs[i].first;
    uint16_t last  = s_runs[i].last;

    s_saved_cmd_idx  = (uint16_t)(first * ROW_PACKET_SIZE);
    s_saved_tail_idx = (uint16_t)((last + 1U) * ROW_PACKET_SIZE + 1U);
    s_saved_cmd      = s_tx_buf[s_saved_cmd_idx];
    memcpy(s_saved_tail, &s_tx_buf[s_saved_tail_idx], RUN_TRAILER_BYTES);
    s_tx_buf[s_saved_cmd_idx] = CMD_WRITE_LINE;
    memset(&s_tx_buf[s_saved_tail_idx], TRAILER, RUN_TRAILER_BYTES);

    uint16_t len = (uint16_t)((last - first + 1U) * ROW_PACKET_SIZE + 1U + RUN_TRAILER_BYTES);
    hal_spi_cs_assert(HAL_SPI_DISPLAY);
    /* SCS setup time before the first clock -- generous margin. */
    hal_systick_delay_us(SCS_SETUP_US);
    hal_spi_write_dma(HAL_SPI_DISPLAY, &s_tx_buf[s_saved_cmd_idx], len);
}

void drv_sharp_lcd_mark_all_dirty(void)
{
    for (uint8_t w = 0; w < DIRTY_WORDS; ++w) {
        s_dirty[w] = 0xFFFFFFFFU;
    }
}

bool drv_sharp_lcd_has_dirty(void)
{
    for (uint16_t r = 0; r < LCD_HEIGHT; ++r) {
        if (row_is_dirty(r)) return true;
    }
    return false;
}

void drv_sharp_lcd_update(void)
{
    if (s_pending_power_settle) {
        /* Non-blocking wait for the display's internal supply to settle
         * after DISP_ON was asserted — s_busy stays true (set in init())
         * the whole time, so any flush attempted meanwhile is correctly
         * refused rather than hitting the wire early. */
        if ((int32_t)(hal_systick_get_ms() - s_settle_deadline_ms) < 0) {
            return;
        }
        s_pending_power_settle = false;
        s_busy                 = false;
        return;   /* next drv_sharp_lcd_flush_dirty() call proceeds normally */
    }

    if (!s_awaiting_drain) {
        return;
    }
    bool idle      = hal_spi_tx_idle(HAL_SPI_DISPLAY);
    bool timed_out = (uint32_t)(hal_systick_get_ms() - s_drain_start_ms) > SPI_DRAIN_TIMEOUT_MS;
    if (!idle && !timed_out) {
        return;   /* still shifting out — check again next pump */
    }
    if (timed_out) {
        /* SPI2 genuinely wedged (SPI_SR_BSY stuck) — a software flag
         * reset alone won't clear that. Cycle the peripheral so the next
         * flush has a real chance of working, and surface the fault
         * rather than silently going dark forever. */
        hal_spi_reinit(HAL_SPI_DISPLAY);
        s_ok = false;
    } else {
        s_ok = true;   /* self-heals once a flush drains cleanly */
    }
    /* Either genuinely drained, or we gave up rather than wedge the
     * display forever (same policy as drv_24lc256.c's write-cycle poll).
     * Hold CS a margin before release either way. */
    hal_systick_delay_us(SCS_HOLD_US);
    hal_spi_cs_deassert(HAL_SPI_DISPLAY);
    s_awaiting_drain = false;
    restore_run_bytes();
    if (timed_out) {
        /* The panel may have received a partial frame: resend everything. */
        drv_sharp_lcd_mark_all_dirty();
        s_busy = false;
        return;
    }
    ++s_run_idx;
    if (s_run_idx < s_run_count) {
        start_run(s_run_idx);   /* s_busy stays set until the last run drains */
        return;
    }
    s_busy = false;
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
    clear_buffer();
}

void drv_sharp_lcd_init(void)
{
    s_busy                 = true;    /* held busy until the settle wait clears it */
    s_awaiting_drain       = false;
    prime_tx_buffer();
    drv_sharp_lcd_mark_all_dirty();   /* panel contents unknown at power-up */

    hal_spi_init(HAL_SPI_DISPLAY);
    hal_spi_register_dma_callback(HAL_SPI_DISPLAY, on_dma_complete);
    s_ok = true;

    /* PD3 VCOM square wave (5 Hz, within the datasheet's 1-10 Hz window) —
     * no hardware PWM channel on REV B, toggled manually in the TIM6
     * period-elapsed ISR (hal_tim.c). */
    hal_tim_vcom_start();

    /* Power up the display and let its internal supply settle before the
     * first write (generous margin over the datasheet's power-up time) —
     * non-blocking: drv_sharp_lcd_update(), pumped from the scheduler,
     * clears s_busy once DISP_ON_SETTLE_US has actually elapsed. The
     * caller's own first draw+flush (app_display_init()) just gets
     * refused with DRV_ERR_NOT_READY until then; the next scheduler tick
     * redraws and sends the real content, so nothing is lost. */
    hal_gpio_set(DISP_ON_PORT, DISP_ON_PIN, true);
    s_settle_deadline_ms   = hal_systick_get_ms() + (DISP_ON_SETTLE_US / 1000U);
    s_pending_power_settle = true;
}

void drv_sharp_lcd_write_row(uint16_t row, const uint8_t *src)
{
    if (row >= LCD_HEIGHT || src == 0) return;
    uint8_t *dst = row_pixels(row);
    if (memcmp(dst, src, LCD_STRIDE) != 0) {
        memcpy(dst, src, LCD_STRIDE);
        s_dirty[row >> 5] |= 1UL << (row & 31U);
    }
}

DrvStatus drv_sharp_lcd_flush_dirty(void)
{
    if (s_busy) return DRV_ERR_NOT_READY;

    /* Build the run list from the dirty bitmap, merging small clean gaps. */
    uint8_t  n    = 0;
    uint16_t r    = 0;
    uint16_t lo   = LCD_HEIGHT;   /* overall span, for the collapse case */
    uint16_t hi   = 0;
    while (r < LCD_HEIGHT) {
        if (!row_is_dirty(r)) { ++r; continue; }
        uint16_t first = r;
        uint16_t last  = r;
        uint16_t gap   = 0;
        for (++r; r < LCD_HEIGHT; ++r) {
            if (row_is_dirty(r)) { last = r; gap = 0; }
            else if (++gap > RUN_MERGE_GAP_ROWS) { break; }
        }
        r = (uint16_t)(last + 1U);
        if (first < lo) lo = first;
        if (last  > hi) hi = last;
        if (n < MAX_RUNS) {
            s_runs[n].first = first;
            s_runs[n].last  = last;
        }
        ++n;
    }
    if (n == 0) return DRV_OK;          /* nothing changed: nothing to send */
    if (n > MAX_RUNS) {
        s_runs[0].first = lo;
        s_runs[0].last  = hi;
        n = 1;
    }

    for (uint8_t w = 0; w < DIRTY_WORDS; ++w) {
        s_dirty[w] = 0;
    }
    s_run_count = n;
    s_run_idx   = 0;
    s_busy      = true;
    start_run(0);
    return DRV_OK;
}

bool drv_sharp_lcd_is_busy(void)
{
    return s_busy;
}

bool drv_sharp_lcd_ok(void)
{
    return s_ok;
}
