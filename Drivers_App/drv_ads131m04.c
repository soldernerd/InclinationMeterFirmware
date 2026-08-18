#include "drv_ads131m04.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_gpio.h"
#include "pin_config.h"
#include "config.h"
#include "stm32g0xx_hal.h"

/* Register addresses used here (datasheet Table 8-12, "Register Map") —
 * only the three registers this driver actually touches are named. */
#define REG_MODE    0x02U
#define REG_CLOCK   0x03U
#define REG_GAIN1   0x04U

/* WREG command word: 011a aaaa annn nnnn -- 011 prefix, 6-bit address,
 * 7-bit (count-1). This driver only ever writes one register at a time
 * (count-1 = 0), so the low 7 bits are always 0. */
#define WREG_CMD(addr)  (uint16_t)(0x6000U | (((addr) & 0x3FU) << 7))

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

static int32_t sign_extend24(uint8_t msb, uint8_t mid, uint8_t lsb)
{
    uint32_t v = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | lsb;
    if (v & 0x00800000UL) {
        v |= 0xFF000000UL;   /* sign-extend bit 23 through bit 31 */
    }
    return (int32_t)v;
}

/* DMA completion callback for the streaming read below -- fires from
 * HAL_SPI_TxRxCpltCallback (hal_spi.c), itself called from the SPI1 DMA
 * ISR. Deasserts CS (asserted just before the transfer was started, in
 * on_trigger() below) and hands the four channel values up to
 * whichever Services-layer module registered via
 * drv_ads131m04_set_on_sample() -- this driver never touches
 * system_state or calls into Services directly (CLAUDE.md 8.1 layering,
 * same reasoning as Drivers_App/drv_rn4871.c's on_config_complete
 * callback). */
static void on_dma_complete(HalSpiInstance instance, bool success)
{
    if (instance != HAL_SPI_ADC) {
        return;
    }
    hal_spi_cs_deassert(HAL_SPI_ADC);
    if (!success || s_on_sample == 0) {
        return;
    }
    /* Frame layout (word index x WORD_BYTES): word0=response(discarded),
     * word1=CH0, word2=CH1, word3=CH2, word4=CH3, word5=CRC(discarded). */
    int32_t ch0 = sign_extend24(s_rx_buf[1 * WORD_BYTES], s_rx_buf[1 * WORD_BYTES + 1], s_rx_buf[1 * WORD_BYTES + 2]);
    int32_t ch1 = sign_extend24(s_rx_buf[2 * WORD_BYTES], s_rx_buf[2 * WORD_BYTES + 1], s_rx_buf[2 * WORD_BYTES + 2]);
    int32_t ch2 = sign_extend24(s_rx_buf[3 * WORD_BYTES], s_rx_buf[3 * WORD_BYTES + 1], s_rx_buf[3 * WORD_BYTES + 2]);
    int32_t ch3 = sign_extend24(s_rx_buf[4 * WORD_BYTES], s_rx_buf[4 * WORD_BYTES + 1], s_rx_buf[4 * WORD_BYTES + 2]);
    s_on_sample(ch0, ch1, ch2, ch3);
}

/* Acquisition trigger -- called from TIM7's interrupt context at
 * exactly the ADC's own sample rate (hal_tim_adc_trigger_start(),
 * registered below). Polls DRDY's GPIO level rather than reacting to
 * its falling edge -- see pin_config.h's ADC_READY_PIN comment for why
 * (PA1/PB1 EXTI1 conflict with the encoder). Given the deterministic,
 * shared-clock relationship between this timer and the ADC's own
 * sample rate, DRDY should always already be low by the time this
 * fires; if it isn't yet (clock start-up transient, jitter), skip this
 * tick rather than reading stale/not-yet-updated data -- self-healing,
 * since the ADC's own DRDY will still be waiting next tick. */
static void on_trigger(void)
{
    if (hal_gpio_get(ADC_READY_PORT, ADC_READY_PIN)) {
        /* DRDY still high -- not ready yet. */
        if (s_dropped_count < UINT16_MAX) {
            s_dropped_count++;
        }
        return;
    }
    if (hal_spi_is_busy(HAL_SPI_ADC)) {
        /* Previous transfer still in flight -- should not happen at this
         * timer rate given FRAME_BYTES take a few microseconds over SPI,
         * but don't stack a second DMA request on top of one already
         * running. */
        if (s_dropped_count < UINT16_MAX) {
            s_dropped_count++;
        }
        return;
    }
    hal_spi_cs_assert(HAL_SPI_ADC);
    if (hal_spi_transmit_receive_dma(HAL_SPI_ADC, s_tx_zero, s_rx_buf, FRAME_BYTES) != DRV_OK) {
        hal_spi_cs_deassert(HAL_SPI_ADC);
        if (s_dropped_count < UINT16_MAX) {
            s_dropped_count++;
        }
    }
}

DrvStatus drv_ads131m04_init(void)
{
    s_on_sample     = 0;
    s_dropped_count = 0;

    hal_spi_init(HAL_SPI_ADC);
    hal_spi_register_dma_callback(HAL_SPI_ADC, on_dma_complete);

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
    HAL_Delay(1);
    hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, true);
    /* t_REGACQ (5 us min) before communicating, per the datasheet's
     * SYNC/RESET Pin section -- same 1 ms margin reasoning as above. */
    HAL_Delay(1);

    if (write_register(REG_CLOCK, CLOCK_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_GAIN1, GAIN1_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_MODE,  MODE_REG_VALUE)  != DRV_OK) return DRV_ERR_COMM;

    hal_tim_adc_trigger_register_callback(on_trigger);
    hal_tim_adc_trigger_start();

    return DRV_OK;
}

void drv_ads131m04_set_on_sample(Ads131m04SampleCb cb)
{
    s_on_sample = cb;
}

uint16_t drv_ads131m04_get_dropped_count(void)
{
    return s_dropped_count;
}
