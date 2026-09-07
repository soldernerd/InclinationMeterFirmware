#ifndef DRV_AD9833_H
#define DRV_AD9833_H

#include "drv_common.h"

/* AD9833 DDS waveform generator (WP7) — programs the chip once, at
 * startup, for continuous sinusoidal output at a fixed frequency (see
 * Config/config.h's AD9833_FREQREG) and starts its MCLK feed. After this
 * returns, the DAC free-runs entirely on-chip: the DDS phase accumulator
 * keeps advancing from MCLK alone, with no further SPI traffic or
 * firmware attention needed (datasheet "Theory of Operation" /
 * "Numerically Controlled Oscillator").
 *
 * Returns DRV_ERR_COMM if any of the five SPI writes that make up the
 * init sequence fails (CLAUDE.md 7.6 — no silent failures) — the caller
 * (main.c) treats this the same as any other boot-critical HAL/driver
 * init failure. */
DrvStatus drv_ad9833_init(void);

/* Put the chip in its lowest-power state — RESET held, internal MCLK
 * path disabled (SLEEP1), DAC powered down (SLEEP12). One SPI3 frame.
 * Power-consumption investigation only; drv_ad9833_init() brings it back.
 * (The external MCLK on PC11 is separate — stop that via
 * hal_tim_dac_clock_stop().) */
DrvStatus drv_ad9833_sleep(void);

#endif /* DRV_AD9833_H */
