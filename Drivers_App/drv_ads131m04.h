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
 * All counters reset at drv_ads131m04_start(). Any one of four faults
 * latches acquisition (on_trigger stops arming reads) and sets
 * fault_code; Services/svc_signal_analysis.c notices it, emits one
 * API2_LOG_ERROR, and stops the pipeline. */
typedef enum {
    ADS_FAULT_NONE     = 0,
    ADS_FAULT_OVERRUN  = 1,   /* SPI DMA lapped the SysTick drain (ring full) */
    ADS_FAULT_FRAMING  = 2,   /* a frame's word 0 != ADS131M04_STATUS_WORD */
    ADS_FAULT_CRC      = 3,   /* a frame's computed CRC != the CRC word the ADS sent */
    ADS_FAULT_SLIP     = 4,   /* frames_produced drifted off the SysTick-clock estimate */
} Ads131m04Fault;

typedef struct {
    uint32_t frames_produced;   /* frames the TIM7 ISR committed to the ring */
    uint32_t frames_drained;    /* frames the SysTick drain consumed */
    uint32_t tim7_fires;        /* trigger-ISR fires this run (info: ISR-miss rate) */
    uint32_t ring_overflow;     /* pushes dropped: SPI DMA a whole ring ahead of the drain */
    uint32_t drain_clamped;     /* drain calls where head-tail exceeded the ring */
    uint32_t framing_err;       /* frames with an unexpected word 0 */
    uint32_t crc_err;           /* frames whose computed CRC != the sent CRC word */
    uint32_t run_ms;            /* elapsed ms since start() (SysTick) */
    int32_t  frame_deficit;     /* (expected - actual) frames since the post-settle
                                 * reference point — see drv_ads131m04_drain_tick() */
    int32_t  frame_deficit_min; /* observed range of frame_deficit */
    int32_t  frame_deficit_max;
    uint16_t drain_clamp_max;   /* largest head-tail gap seen */
    uint16_t word0_last;        /* most recent frame's word 0 (STATUS response) */
    uint16_t crc_rx_last;       /* most recent frame's CRC word (as the ADS sent it) */
    uint16_t crc_calc_last;     /* CRC computed over that frame's words 0..4 */
    uint8_t  fault_code;        /* Ads131m04Fault — 0 while healthy */
} Ads131m04Integrity;

const Ads131m04Integrity *drv_ads131m04_get_integrity(void);

/* True once acquisition has latched on an integrity fault (fault_code
 * != 0). The pipeline has stopped arming reads; a full stop()/start()
 * cycle clears it. */
bool drv_ads131m04_faulted(void);

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
