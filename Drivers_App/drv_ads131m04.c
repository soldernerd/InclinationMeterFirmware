#include "drv_ads131m04.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "pin_config.h"
#include "config.h"

/* Register addresses used here (datasheet Table 8-12, "Register Map"). */
#define REG_ID      0x00U
#define REG_STATUS  0x01U
#define REG_MODE    0x02U
#define REG_CLOCK   0x03U
#define REG_GAIN1   0x04U
#define REG_CFG     0x06U

/* WREG command word: 011a aaaa annn nnnn -- 011 prefix, 6-bit address,
 * 7-bit (count-1). This driver only ever writes one register at a time
 * (count-1 = 0), so the low 7 bits are always 0. */
#define WREG_CMD(addr)  (uint16_t)(0x6000U | (((addr) & 0x3FU) << 7))

/* RREG command word: 101a aaaa annn nnnn -- 101 prefix, same addr/count
 * fields. The register contents come back in word 0 of the NEXT frame. */
#define RREG_CMD(addr)  (uint16_t)(0xA000U | (((addr) & 0x3FU) << 7))

/* CLOCK register (datasheet Table 8-17): CH3_EN..CH0_EN=1 (all four
 * channels), TBM=0, OSR[2:0]=ADS131M04_OSR_FIELD (config.h — 000b = 128,
 * chosen for fDATA = fCLKIN/256), PWR[1:0]=10b (high-resolution mode,
 * required for fCLKIN=5.3333 MHz -- see the datasheet's Recommended
 * Operating Conditions, high-resolution mode's fCLKIN range is
 * 0.3-8.4 MHz). Reset default is 0F0Eh; this changes only the OSR field. */
#define CLOCK_REG_VALUE  (uint16_t)(0x0F00U | (ADS131M04_OSR_FIELD << 2) | 0x2U)

/* GAIN1 register (datasheet Table 8-18): PGAGAIN0..3[2:0] = 000b (gain
 * = 1) for all four channels -- this is already the POR/reset default
 * (0000h), written explicitly anyway for clarity and to not silently
 * depend on the reset value never changing. */
#define GAIN1_REG_VALUE  0x0000U

/* MODE register (datasheet Table 8-16): same as the 0510h reset default
 * except RESET (bit 10) cleared to 0 -- our own SYNC_RESET pulse below
 * sets this flag, and it must be explicitly cleared (writing 0 is the
 * documented way to clear the STATUS register's RESET bit). WLENGTH
 * stays at its default 01b (24-bit words, matching the ADC's native
 * resolution -- see the frame-buffer sizing below). */
#define MODE_REG_VALUE   0x0110U

/* On-the-wire frame size with WLENGTH=24-bit (default): 6 words x 3
 * bytes = [response][ch0][ch1][ch2][ch3][crc]. Datasheet "SPI
 * Communication Frames" -- fixed length regardless of command, the
 * device always shifts out its full response. */
#define FRAME_BYTES  18U
#define WORD_BYTES   3U

static Ads131m04SampleCb s_on_sample = 0;
static uint16_t          s_dropped_count = 0;
static bool              s_running = false;
static bool              s_xfer_active = false;
static Ads131m04Regs     s_regs;

static const uint8_t s_tx_zero[FRAME_BYTES] = { 0 };   /* NULL command, no CRC */
static uint8_t       s_rx_buf[FRAME_BYTES];

/* Blocking, TX-only, single-register write -- used only during the
 * one-time init sequence below, never during streaming. Ignores
 * whatever the ADC shifts back on DOUT (settling/meaningless data
 * before OSR and gain are configured) -- acceptable for a boot-only
 * register write, unlike the streaming path which must capture every
 * response. Always sends a full FRAME_BYTES-sized frame: the device's
 * own output shift register needs that many clocks regardless of how
 * short the actual command is, per the datasheet's frame-length
 * description. */
static DrvStatus write_register(uint8_t addr, uint16_t value)
{
    uint8_t buf[FRAME_BYTES] = { 0 };
    uint16_t cmd = WREG_CMD(addr);

    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFFU);
    /* buf[2] = 0 -- 24-bit word LSB padding */
    buf[3] = (uint8_t)(value >> 8);
    buf[4] = (uint8_t)(value & 0xFFU);
    /* buf[5] = 0 -- 24-bit word LSB padding; remaining bytes already 0 */

    hal_spi_cs_assert(HAL_SPI_ADC);
    DrvStatus rc = hal_spi_write(HAL_SPI_ADC, buf, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    return rc;
}

/* Blocking single-register read. RREG in one frame, then a NULL frame to
 * clock the response out of word 0. Init/diagnostic use only. The 16-bit
 * register value sits in the top 16 bits of the 24-bit response word. */
static DrvStatus read_register(uint8_t addr, uint16_t *value)
{
    uint8_t tx[FRAME_BYTES] = { 0 };
    uint8_t rx[FRAME_BYTES] = { 0 };
    uint16_t cmd = RREG_CMD(addr);

    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFFU);

    hal_spi_cs_assert(HAL_SPI_ADC);
    DrvStatus rc = hal_spi_transmit_receive(HAL_SPI_ADC, tx, rx, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    if (rc != DRV_OK) return rc;

    uint8_t nul[FRAME_BYTES] = { 0 };
    uint8_t resp[FRAME_BYTES] = { 0 };
    hal_spi_cs_assert(HAL_SPI_ADC);
    rc = hal_spi_transmit_receive(HAL_SPI_ADC, nul, resp, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    if (rc != DRV_OK) return rc;

    *value = (uint16_t)(((uint16_t)resp[0] << 8) | resp[1]);
    return DRV_OK;
}

static void read_all_registers(void)
{
    s_regs.clock_expected = CLOCK_REG_VALUE;
    bool ok = true;
    ok &= (read_register(REG_ID,     &s_regs.id)     == DRV_OK);
    ok &= (read_register(REG_STATUS, &s_regs.status) == DRV_OK);
    ok &= (read_register(REG_MODE,   &s_regs.mode)   == DRV_OK);
    ok &= (read_register(REG_CLOCK,  &s_regs.clock)  == DRV_OK);
    ok &= (read_register(REG_GAIN1,  &s_regs.gain1)  == DRV_OK);
    ok &= (read_register(REG_CFG,    &s_regs.cfg)    == DRV_OK);
    s_regs.read_ok = ok;
}

const Ads131m04Regs *drv_ads131m04_get_regs(void)
{
    return &s_regs;
}

static int32_t sign_extend24(uint8_t msb, uint8_t mid, uint8_t lsb)
{
    uint32_t v = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | lsb;
    if (v & 0x00800000UL) {
        v |= 0xFF000000UL;   /* sign-extend bit 23 through bit 31 */
    }
    return (int32_t)v;
}

/* DRDY poll + raw-DMA frame read -- called from TIM7's lean ISR at 2x the
 * ADC data rate (config.h ADS131M04_TRIGGER_TIMER_PERIOD). DRDY is polled
 * as a level, not an edge (PA1/PB1 EXTI1 conflicts with the encoder --
 * pin_config.h's ADC_READY_PIN comment); in MODE register DRDY_FMT=0 it
 * stays low from the end of a conversion until the frame is read, so
 * "DRDY low" == "an unread conversion is waiting".
 *
 * State machine, one step per tick:
 *   - a frame in flight and finished  -> collect it, deassert CS
 *   - a frame in flight, not finished -> nothing to do this tick
 *   - idle and DRDY low               -> kick a new frame
 * The read is raw DMA (hal_spi_adc_stream_*), ~10 us wall and almost no
 * CPU, so at 2x oversample the frame always completes within one tick and
 * every conversion is read exactly once -- uniform sampling at fDATA. */
static void on_trigger(void)
{
    if (!s_running) {
        return;
    }

    if (s_xfer_active) {
        if (!hal_spi_adc_stream_done()) {
            return;                       /* still shifting */
        }
        hal_spi_adc_stream_end();
        hal_spi_cs_deassert(HAL_SPI_ADC);
        s_xfer_active = false;

        if (s_on_sample != 0) {
            /* word0=response, word1..4 = CH0..CH3, word5 = CRC. */
            int32_t ch0 = sign_extend24(s_rx_buf[1 * WORD_BYTES], s_rx_buf[1 * WORD_BYTES + 1], s_rx_buf[1 * WORD_BYTES + 2]);
            int32_t ch1 = sign_extend24(s_rx_buf[2 * WORD_BYTES], s_rx_buf[2 * WORD_BYTES + 1], s_rx_buf[2 * WORD_BYTES + 2]);
            int32_t ch2 = sign_extend24(s_rx_buf[3 * WORD_BYTES], s_rx_buf[3 * WORD_BYTES + 1], s_rx_buf[3 * WORD_BYTES + 2]);
            int32_t ch3 = sign_extend24(s_rx_buf[4 * WORD_BYTES], s_rx_buf[4 * WORD_BYTES + 1], s_rx_buf[4 * WORD_BYTES + 2]);
            s_on_sample(ch0, ch1, ch2, ch3);
        }
    }

    if (hal_gpio_get(ADC_READY_PORT, ADC_READY_PIN)) {
        return;                           /* DRDY high -- nothing new */
    }

    hal_spi_cs_assert(HAL_SPI_ADC);
    hal_spi_adc_stream_begin(s_tx_zero, s_rx_buf, FRAME_BYTES);
    s_xfer_active = true;
}

DrvStatus drv_ads131m04_init(void)
{
    s_on_sample     = 0;
    s_dropped_count = 0;
    s_running       = false;
    s_xfer_active   = false;

    hal_spi_init(HAL_SPI_ADC);

    /* MCLK first -- the ADC's internal logic (including SYNC/RESET
     * handling below) is synchronous to it, same reasoning as the DAC's
     * drv_ad9833_init(). */
    hal_tim_adc_clock_start();

    /* Explicit hardware reset via SYNC/RESET rather than relying solely
     * on POR: guarantees a known starting state regardless of whatever
     * happened before this boot. Idle HIGH (see pin_config.h); pulse LOW
     * for >=2048 CLKIN cycles (datasheet t_w(RSL), ~384 us at our 5.3333
     * MHz CLKIN) -- 1 ms is a generous, simple margin for a one-time
     * boot operation, not a tight budget worth hand-timing. */
    hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, false);
    hal_systick_delay_ms(1);
    hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, true);
    /* t_REGACQ (5 us min) before communicating, per the datasheet's
     * SYNC/RESET Pin section -- same 1 ms margin reasoning as above. */
    hal_systick_delay_ms(1);

    if (write_register(REG_CLOCK, CLOCK_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_GAIN1, GAIN1_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_MODE,  MODE_REG_VALUE)  != DRV_OK) return DRV_ERR_COMM;

    /* Read the config back so a host can see whether the WREGs actually
     * landed (drv_ads131m04_get_regs()). */
    read_all_registers();

    /* Hand SPI1 + its DMA channels over to the raw streaming path (no more
     * blocking/HAL-SPI calls on this bus after this point). */
    hal_spi_adc_stream_init();

    /* Trigger callback is registered here but the timer is left stopped —
     * drv_ads131m04_start() arms it. See the header comment. */
    hal_tim_adc_trigger_register_callback(on_trigger);

    return DRV_OK;
}

DrvStatus drv_ads131m04_start(void)
{
    if (s_running) {
        return DRV_OK;
    }
    s_dropped_count = 0;
    s_xfer_active   = false;
    s_running = true;
    hal_tim_adc_trigger_start();
    return DRV_OK;
}

void drv_ads131m04_stop(void)
{
    if (!s_running) {
        return;
    }
    hal_tim_adc_trigger_stop();
    s_running = false;
    /* Let any frame kicked from on_trigger() drain before we park CS —
     * bounded wait, task context only. A raw frame is ~10 us on the wire. */
    for (uint32_t i = 0; i < 100000U && s_xfer_active && !hal_spi_adc_stream_done(); ++i) {
        __asm volatile("nop");
    }
    if (s_xfer_active) {
        hal_spi_adc_stream_end();
        s_xfer_active = false;
    }
    hal_spi_cs_deassert(HAL_SPI_ADC);
}

bool drv_ads131m04_is_running(void)
{
    return s_running;
}

void drv_ads131m04_set_on_sample(Ads131m04SampleCb cb)
{
    s_on_sample = cb;
}

uint16_t drv_ads131m04_get_dropped_count(void)
{
    return s_dropped_count;
}
