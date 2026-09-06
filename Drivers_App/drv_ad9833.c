#include "drv_ad9833.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include "config.h"
#include <stdint.h>

/* AD9833 16-bit control-word field layout (datasheet Table II / Figure 5,
 * "Description of Bits in the Control Register"). Only the bits this
 * driver actually sets are named — everything else is left at its
 * required-zero default by the plain |-combinations below. */
#define CTRL_B28        (1U << 13)   /* D13 — full 28-bit freq write, as two 14-bit halves */
#define CTRL_RESET      (1U << 8)    /* D8  — 1 = hold at reset (midscale, no accumulation) */

/* D15:D14 select which register a write targets (datasheet Table IV /
 * "Writing to a Phase Register"). FREQ0/PHASE0 only — FSELECT/PSELECT
 * both default to 0, so this driver never touches FREQ1/PHASE1. */
#define FREQ0_WRITE     (0x4000U)    /* D15:D14 = 01 */
#define PHASE0_WRITE    (0xC000U)    /* D15:D14 = 11, D13 = 0 (PHASE0, not PHASE1) */

/* Gap between consecutive 16-bit frames. FSYNC is a plain GPIO on PC13,
 * one of the STM32's limited-drive I/Os with a slow slew — give it time
 * to return fully HIGH and any ringing to die before the next frame, so
 * the AD9833 can't see a spurious mid-frame FSYNC edge and split a word.
 * (Bench symptom without this: ~1 boot in a few came up at fOUT/8,
 * i.e. a garbled FREQ0 MSB half; a power-cycle — sometimes several —
 * fixed it.) */
#define FRAME_GAP_US    20U

static DrvStatus write_word(uint16_t word)
{
    hal_spi_cs_assert(HAL_SPI_DAC);
    DrvStatus rc = hal_spi_write(HAL_SPI_DAC, (const uint8_t *)&word, 1);
    hal_spi_cs_deassert(HAL_SPI_DAC);
    hal_systick_delay_us(FRAME_GAP_US);
    return rc;
}

/* Full register load, idempotent (every write is an absolute value).
 * The leading CTRL_B28 write also resets the AD9833's internal
 * LSB-then-MSB toggle, so the FREQ0 pair is always interpreted in the
 * right order even if a previous (possibly corrupted) frame left that
 * toggle mid-sequence. */
static DrvStatus load_registers(void)
{
    /* Hold RESET (analog output at midscale) while FREQ0/PHASE0 load, to
     * avoid an output transient during setup — datasheet "Powering Up
     * the AD9833". B28=1: 28-bit freq written as two 14-bit halves. */
    if (write_word(CTRL_B28 | CTRL_RESET) != DRV_OK) return DRV_ERR_COMM;

    /* FREQ0 = AD9833_FREQREG (config.h): 14 LSBs first, then 14 MSBs —
     * order is mandatory per the datasheet's "Writing to a Frequency
     * Register". */
    if (write_word(FREQ0_WRITE | (uint16_t)(AD9833_FREQREG & 0x3FFFUL)) != DRV_OK) return DRV_ERR_COMM;
    if (write_word(FREQ0_WRITE | (uint16_t)((AD9833_FREQREG >> 14) & 0x3FFFUL)) != DRV_OK) return DRV_ERR_COMM;

    /* PHASE0 = 0. RESET does not clear the phase/frequency/control
     * registers — they power up with invalid data and must be written. */
    if (write_word(PHASE0_WRITE | 0U) != DRV_OK) return DRV_ERR_COMM;

    /* Release RESET. SLEEP1/SLEEP12 (MCLK + DAC active), OPBITEN/MODE
     * (sinusoidal to VOUT) all stay at their required-0 default by not
     * being set. Output appears ~8 MCLK cycles later, entirely on-chip. */
    if (write_word(CTRL_B28) != DRV_OK) return DRV_ERR_COMM;

    return DRV_OK;
}

DrvStatus drv_ad9833_init(void)
{
    hal_spi_init(HAL_SPI_DAC);

    /* MCLK first, then let it settle. RESET and the register loads below
     * are internally synchronous to MCLK (datasheet "RESET Function" /
     * "Latency"), so it must be running and stable before any of them.
     * Left running forever afterward; see hal_tim_dac_clock_start(). */
    hal_tim_dac_clock_start();
    hal_systick_delay_us(2000U);

    /* Load the registers twice. A single frame corrupted by FSYNC noise
     * on the first pass is simply overwritten on the second (all writes
     * are absolute). ~1 ms total, one-shot at boot — no hot path. The
     * caller (main.c) reports the result to g_system_state.dac_ok. */
    DrvStatus rc1 = load_registers();
    hal_systick_delay_us(500U);
    DrvStatus rc2 = load_registers();

    return (rc2 == DRV_OK) ? DRV_OK : rc1;
}
