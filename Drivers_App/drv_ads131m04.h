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

/* Fired once per sample. As of the ADC_Optimization work this runs in the
 * SysTick drain, not the TIM7 ISR: the SPI RX DMA writes each frame
 * straight into a ring slot, the trigger ISR just advances head, and
 * SysTick does the sign-extend + this callback in batches
 * (docs/adc_acquisition_redesign.md). Still keep the callback short — no
 * blocking calls. Values are raw two's-
 * complement 24-bit ADC codes, sign-extended to int32_t (datasheet "ADC
 * Conversion Data" — 1 LSB = 2.4 V / Gain / 2^24, Gain = 1). Channel-to-
 * voltage and further analysis belong above this driver (CLAUDE.md 8.1) —
 * see Services/svc_signal_analysis.c. */
typedef void (*Ads131m04SampleCb)(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3);
void drv_ads131m04_set_on_sample(Ads131m04SampleCb cb);

/* Runs the frame-ring drain (sign-extend + per-sample callback for every
 * queued frame). Call site: Core/Src/stm32g0xx_it.c's SysTick_Handler,
 * once per ms. No-op unless acquisition is running. */
void drv_ads131m04_drain_tick(void);

/* --- acquisition integrity (docs/adc_acquisition_redesign.md) ---
 * Reset at drv_ads131m04_start(). Phase 1 is mostly observational: the
 * counters are live, word0_sample/crc_rx_last capture on-the-wire
 * behaviour so Phase 2 can turn CRC + conversion-count checks into a
 * latching ERROR gate. */
typedef struct {
    uint32_t frames_produced;   /* frames the TIM7 ISR pushed to the ring */
    uint32_t frames_drained;    /* frames the SysTick drain consumed */
    uint32_t ring_overflow;     /* pushes dropped: drain a whole ring behind */
    uint32_t drain_clamped;     /* drain calls where head-tail exceeded the ring */
    uint16_t drain_clamp_max;   /* largest head-tail gap seen */
    uint16_t word0_sample[8];   /* first frames' response/STATUS word this run */
    uint8_t  word0_count;
    uint16_t crc_rx_last;       /* most recent frame's CRC word (top 16 bits) */
} Ads131m04Integrity;

const Ads131m04Integrity *drv_ads131m04_get_integrity(void);

/* Saturating count of trigger ticks where DRDY was not yet low (sample
 * skipped rather than read) — CLAUDE.md 7.6 escalation for a case that
 * should not occur in practice given the deterministic clock
 * relationship, but is not otherwise flagged anywhere (see
 * pin_config.h's ADC_READY_PIN comment for why this is polled rather
 * than interrupt-driven). */
uint16_t drv_ads131m04_get_dropped_count(void);

/* --- register read-back diagnostics ---
 * Snapshot of the ADS131M04's config registers, read back over SPI at
 * the end of drv_ads131m04_init() (right after the WREG sequence). Lets
 * a host confirm the OSR / mode actually took — the sample rate we saw
 * on the bench was well below fCLKIN/256, which points at a WREG that
 * isn't landing. `read_ok` is false if any RREG transfer failed.
 * `clock_expected` is what the driver tried to write to CLOCK. */
typedef struct {
    uint16_t id;
    uint16_t status;
    uint16_t mode;
    uint16_t clock;
    uint16_t gain1;
    uint16_t cfg;
    uint16_t clock_expected;
    bool     read_ok;
} Ads131m04Regs;

const Ads131m04Regs *drv_ads131m04_get_regs(void);

#endif /* DRV_ADS131M04_H */
