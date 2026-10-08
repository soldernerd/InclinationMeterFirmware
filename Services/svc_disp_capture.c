#include "svc_displacement.h"
#include "svc_displacement_internal.h"
#include "drv_ads131m04.h"
#include "hal_systick.h"
#include "config.h"
#include <stddef.h>

/* Bulk raw-ADC capture (API Bulk 0x8), split out of svc_displacement.c. Fills a RAM buffer with
 * the raw 24-bit codes at the full sample rate while the demodulation is not running; the API's
 * bulk pump reads it out afterwards. See svc_displacement.h for the contract. */

/* disp_capture_store() runs inside the ADC drain, the same hot path as the sample callback --
 * built -O2 with it (CMakeLists.txt). */
#if !defined(__OPTIMIZE__)
#error "hot-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

/* --- Bulk raw-ADC capture buffer (restored 2026-09-25, see
 * svc_displacement.h's comment) --- packed 24-bit codes, little-endian
 * signed, ch0..ch3 interleaved: 12 bytes per sample. Filled from
 * on_sample() (the same SysTick-frame-drain context, not a raw ISR) while
 * g_disp_cap_active; drained by svc_api's bulk pump (task) once s_cap_done. */
static uint8_t           s_cap_buf[ADC_BULK_SAMPLE_COUNT * ADC_BULK_BYTES_PER_SAMPLE];
static volatile uint16_t s_cap_idx    = 0;
volatile bool            g_disp_cap_active = false;
static volatile bool     s_cap_done   = false;
static volatile uint32_t s_cap_t0     = 0;   /* tick at capture_begin() */
static volatile uint32_t s_cap_t1     = 0;   /* tick when the buffer filled */

/* Stats of the most recently completed capture — held past capture_end()
 * so a host diagnostic can read them back (effective sample rate =
 * samples / elapsed_ms). s_last_drops is the acquisition ring-overflow
 * count (drain fell a whole ring behind) accumulated during the fill —
 * the only lost-sample mechanism a full-rate capture has. */
static uint16_t s_last_samples    = 0;
static uint16_t s_last_drops      = 0;
static uint32_t s_last_elapsed_ms = 0;

void disp_capture_store(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3)
{
    if (s_cap_idx < ADC_BULK_SAMPLE_COUNT) {
        uint8_t *p = &s_cap_buf[(size_t)s_cap_idx * ADC_BULK_BYTES_PER_SAMPLE];
        const int32_t v[4] = { ch0, ch1, ch2, ch3 };
        for (uint8_t i = 0; i < 4U; ++i) {
            p[i * 3 + 0] = (uint8_t)(v[i] & 0xFF);
            p[i * 3 + 1] = (uint8_t)((v[i] >> 8) & 0xFF);
            p[i * 3 + 2] = (uint8_t)((v[i] >> 16) & 0xFF);
        }
        if (++s_cap_idx >= ADC_BULK_SAMPLE_COUNT) {
            s_cap_t1   = hal_systick_get_ms();
            s_cap_done = true;
        }
    }
}

DrvStatus svc_displacement_capture_begin(void)
{
    if (g_disp_cap_active) {
        return DRV_ERR_NOT_READY;
    }
    s_cap_idx    = 0;
    s_cap_done   = false;
    s_cap_t1     = 0;
    s_cap_t0     = hal_systick_get_ms();
    g_disp_cap_active = true;   /* on_sample() now stores into s_cap_buf */
    return drv_ads131m04_start();
}

bool svc_displacement_capture_done(void)
{
    return s_cap_done;
}

void svc_displacement_capture_end(void)
{
    if (g_disp_cap_active) {
        uint32_t t1 = s_cap_t1 ? s_cap_t1 : hal_systick_get_ms();
        s_last_samples    = s_cap_idx;
        s_last_drops      = (uint16_t)drv_ads131m04_get_integrity()->ring_overflow;
        s_last_elapsed_ms = t1 - s_cap_t0;
    }
    g_disp_cap_active = false;
    drv_ads131m04_stop();
}

void svc_displacement_last_capture(uint16_t *samples, uint16_t *drops,
                                    uint32_t *elapsed_ms)
{
    if (samples)    *samples    = s_last_samples;
    if (drops)      *drops      = s_last_drops;
    if (elapsed_ms) *elapsed_ms = s_last_elapsed_ms;
}

const uint8_t *svc_displacement_capture_buffer(void)
{
    return s_cap_buf;
}

uint16_t svc_displacement_capture_sample_count(void)
{
    return ADC_BULK_SAMPLE_COUNT;
}

uint16_t svc_displacement_capture_drops(void)
{
    return (uint16_t)drv_ads131m04_get_integrity()->ring_overflow;
}
