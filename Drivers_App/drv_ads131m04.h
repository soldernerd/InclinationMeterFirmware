#ifndef DRV_ADS131M04_H
#define DRV_ADS131M04_H

#include <stdint.h>
#include "drv_common.h"

/* Configures the ADS131M04 for continuous 4-channel simultaneous
 * sampling at a fixed ~20833.33 Hz (see Config/config.h's
 * ADS131M04_OSR_FIELD), PGA gain = 1 on all channels, and starts both
 * its MCLK feed and the acquisition trigger timer. Call
 * drv_ads131m04_set_on_sample() first so samples aren't silently
 * dropped from the moment streaming starts. */
DrvStatus drv_ads131m04_init(void);

/* Fired once per sample, from the acquisition trigger's interrupt
 * context (HAL_App/hal_tim.c's TIM7 callback -> this driver's DMA
 * completion handler) — keep the callback itself short (no blocking
 * calls). Values are raw two's-complement 24-bit ADC codes, sign-
 * extended to int32_t (datasheet "ADC Conversion Data" — 1 LSB =
 * 2.4 V / Gain / 2^24, and Gain = 1 here). Channel-to-voltage and any
 * further signal analysis belongs above this driver, per CLAUDE.md 8.1 —
 * see Services/svc_displacement.c. */
typedef void (*Ads131m04SampleCb)(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3);
void drv_ads131m04_set_on_sample(Ads131m04SampleCb cb);

/* Saturating count of trigger ticks where DRDY was not yet low (sample
 * skipped rather than read) — CLAUDE.md 7.6 escalation for a case that
 * should not occur in practice given the deterministic clock
 * relationship, but is not otherwise flagged anywhere (see
 * pin_config.h's ADC_READY_PIN comment for why this is polled rather
 * than interrupt-driven). */
uint16_t drv_ads131m04_get_dropped_count(void);

#endif /* DRV_ADS131M04_H */
