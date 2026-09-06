#ifndef DRV_ADS131M04_H
#define DRV_ADS131M04_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Configures the ADS131M04 for continuous 4-channel simultaneous
 * sampling at a fixed ~20833.33 Hz (see Config/config.h's
 * ADS131M04_OSR_FIELD), PGA gain = 1 on all channels, and starts its
 * MCLK feed. Does NOT start the acquisition trigger — call
 * drv_ads131m04_start() for that once a consumer is ready. Resets the
 * per-sample callback to none; call drv_ads131m04_set_on_sample() AFTER
 * this (and before drv_ads131m04_start()) so samples aren't silently
 * dropped from the moment streaming begins. */
DrvStatus drv_ads131m04_init(void);

/* Start / stop the 20833 Hz acquisition trigger. Split out from _init()
 * in v0.8.2: the sample stream feeds only Services/svc_signal_analysis.c,
 * which has no consumer of its own output yet, and running the pipeline
 * unconditionally at boot starved the cooperative scheduler's SysTick
 * (erratic status LED / 1-2 s stalls). Both are cheap and idempotent;
 * _stop() also parks CS deasserted. Toggled at runtime over the API
 * (API2_RES_CMD_SIGNAL_ANALYSIS). Not interrupt-safe — call from task
 * context only. */
DrvStatus drv_ads131m04_start(void);
void      drv_ads131m04_stop(void);
bool      drv_ads131m04_is_running(void);

/* Fired once per sample, from the acquisition trigger's interrupt
 * context (HAL_App/hal_tim.c's TIM7 callback -> this driver's DMA
 * completion handler) — keep the callback itself short (no blocking
 * calls). Values are raw two's-complement 24-bit ADC codes, sign-
 * extended to int32_t (datasheet "ADC Conversion Data" — 1 LSB =
 * 2.4 V / Gain / 2^24, and Gain = 1 here). Channel-to-voltage and any
 * further signal analysis belongs above this driver, per CLAUDE.md 8.1 —
 * see Services/svc_signal_analysis.c. */
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
