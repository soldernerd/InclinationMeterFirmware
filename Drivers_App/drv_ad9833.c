#include "drv_ad9833.h"
#include "hal_spi.h"
#include "hal_tim.h"
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

static DrvStatus write_word(uint16_t word)
{
    hal_spi_cs_assert(HAL_SPI_DAC);
    DrvStatus rc = hal_spi_write(HAL_SPI_DAC, (const uint8_t *)&word, 1);
    hal_spi_cs_deassert(HAL_SPI_DAC);
    return rc;
}

DrvStatus drv_ad9833_init(void)
{
    hal_spi_init(HAL_SPI_DAC);

    /* MCLK first: RESET and the register loads below are internally
     * synchronous to MCLK (datasheet "RESET Function" / "Latency"), so
     * it needs to already be running for them to take effect cleanly —
     * unlike the SPI writes themselves, which are clocked by SCLK and
     * asynchronous to MCLK. Left running forever afterward; see
     * hal_tim_dac_clock_start()'s own comment. */
    hal_tim_dac_clock_start();

    /* Hold the DAC in reset (analog output forced to midscale) while the
     * frequency/phase registers are loaded, to avoid a spurious output
     * transient during setup — datasheet "Powering Up the AD9833". */
    if (write_word(CTRL_B28 | CTRL_RESET) != DRV_OK) return DRV_ERR_COMM;

    /* FREQ0 = AD9833_FREQREG (config.h), written as its 14 LSBs then its
     * 14 MSBs (B28=1 two-write mode) — LSBs MUST be written first, per
     * the datasheet's "Writing to a Frequency Register": "the first
     * write will contain the 14 LSBs, while the second write will
     * contain the 14 MSBs". */
    if (write_word(FREQ0_WRITE | (uint16_t)(AD9833_FREQREG & 0x3FFFUL)) != DRV_OK) return DRV_ERR_COMM;
    if (write_word(FREQ0_WRITE | (uint16_t)((AD9833_FREQREG >> 14) & 0x3FFFUL)) != DRV_OK) return DRV_ERR_COMM;

    /* PHASE0 = 0 (no phase offset). RESET does not clear the phase,
     * frequency, or control registers — they power up with invalid
     * data and must be explicitly written, per the same "Powering Up
     * the AD9833" section. */
    if (write_word(PHASE0_WRITE | 0U) != DRV_OK) return DRV_ERR_COMM;

    /* Release RESET. FSELECT/PSELECT (FREQ0/PHASE0), SLEEP1/SLEEP12
     * (MCLK + DAC both active), and OPBITEN/MODE (sinusoidal output
     * routed to VOUT) all default to their required-0 state simply by
     * not being set here. The analog output appears ~8 MCLK cycles
     * later, entirely on-chip — no further action needed, per
     * drv_ad9833.h. */
    if (write_word(CTRL_B28) != DRV_OK) return DRV_ERR_COMM;

    return DRV_OK;
}
