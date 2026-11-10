#include "math_quality.h"
#include <string.h>
#include <math.h>

/* Fresh blend state: the next output is the fast window and fades to the slow one. */
static void blend_restart(MathDisplay *d)
{
    float s0 = (d->blend_k > 0.0f) ? d->blend_min / d->blend_k : 0.0f;
    d->blend_sigma2[0] = d->blend_sigma2[1] = s0 * s0;
    d->blend_w[0] = d->blend_w[1] = 1.0f;
}

void math_display_set_blend(MathDisplay *d, float blend_min, float blend_k, float sigma_alpha, float hold)
{
    d->blend_min   = blend_min;
    d->blend_k     = blend_k;
    d->blend_alpha = sigma_alpha;
    d->blend_hold  = hold;
    blend_restart(d);
}

void math_display_reset(MathDisplay *d)
{
    math_window_reset(&d->win, d->creep, d->reseed_batches);
    d->phase = 0;
    d->out[0] = d->out[1] = 0.0f;
    d->doubtful[0] = d->doubtful[1] = false;
    d->valid = false;
    d->have_seq = false;
    blend_restart(d);
    d->seq++;
}

void math_display_init(MathDisplay *d, uint16_t fast_taps, uint16_t slow_taps, uint16_t quality_taps,
                       uint16_t decimation, uint16_t prec_window,
                       float k, float creep, uint32_t reseed_batches)
{
    memset(d, 0, sizeof *d);
    d->fast_taps      = fast_taps;
    d->slow_taps      = slow_taps;
    d->quality_taps   = quality_taps;
    d->decimation     = decimation;
    d->prec_window    = prec_window;
    d->k              = k;
    d->creep          = creep;
    d->reseed_batches = reseed_batches;
    d->hann_fast_sum  = math_hann_weights(d->hann_fast, fast_taps);
    d->hann_slow_sum  = math_hann_weights(d->hann_slow, slow_taps);
    d->hann_prec_sum  = math_hann_weights(d->hann_prec, prec_window);
    math_window_reset(&d->win, creep, reseed_batches);   /* seq stays 0 until the first reset/publish */
    math_display_set_blend(d, 0.0f, 6.0f, 1.0f / 240.0f, 0.78f);
}

void math_display_skip(MathDisplay *d)
{
    d->have_seq = false;
    d->valid = false;
}

bool math_display_feed(MathDisplay *d, uint16_t seq, uint16_t expected_step,
                       const float delta[2], const float residual[2])
{
    bool contiguous = d->have_seq && (uint16_t)(seq - d->last_seq) == expected_step;
    if (!contiguous) {
        math_window_break(&d->win);
        d->phase = 0;
        d->blend_w[0] = d->blend_w[1] = 1.0f;   /* the slow window starts empty: fast, then fade */
    }
    d->last_seq = seq;
    d->have_seq = true;

    math_window_push(&d->win, delta, residual);

    if (++d->phase < d->decimation) {
        return contiguous;
    }
    d->phase = 0;
    const uint16_t first = (d->fast_taps > d->quality_taps) ? d->fast_taps : d->quality_taps;
    if (d->win.count < first) {
        return contiguous;   /* windows not yet full */
    }
    const bool slow_ready = d->win.count >= d->slow_taps;
    for (uint8_t s = 0; s < 2U; ++s) {
        const float fast = math_window_hann_mean(&d->win, s, d->fast_taps, d->hann_fast, d->hann_fast_sum);
        float out = fast;
        if (slow_ready) {
            const float slow = math_window_hann_mean(&d->win, s, d->slow_taps, d->hann_slow, d->hann_slow_sum);
            const float diff = fast - slow;
            const float ad   = fabsf(diff);
            float thr = d->blend_k * sqrtf(d->blend_sigma2[s]);
            if (thr < d->blend_min) {
                thr = d->blend_min;
            }
            /* noise estimate: the mean square of the difference, each sample clipped at the threshold (winsorized), so
             * a real move cannot inflate it by more than the threshold, while a noisy site (many samples at the clip)
             * still raises it step by step -- a censored estimate (ignoring samples above the threshold) never
             * recovered from heavy-tailed noise and left the display in the noisy fast window on a busy day */
            const float clipped = (ad < thr) ? diff * diff : thr * thr;
            d->blend_sigma2[s] += d->blend_alpha * (clipped - d->blend_sigma2[s]);
            float w = (thr > 0.0f) ? (ad - thr) / thr : 1.0f;   /* 0 at the threshold, 1 at twice it */
            if (w > 1.0f) w = 1.0f;
            if (w < 0.0f) w = 0.0f;
            const float held = d->blend_w[s] * d->blend_hold;   /* fades out after the movement stops */
            d->blend_w[s] = (w > held) ? w : held;
            out = slow + d->blend_w[s] * diff;
        } else {
            d->blend_w[s] = 1.0f;
        }
        d->out[s]      = out;
        d->doubtful[s] = !math_window_clean_sensor(&d->win, s, d->quality_taps, d->k);
    }
    d->valid = true;
    d->seq++;
    return contiguous;
}

void math_precision_start(MathPrecision *p)
{
    memset(p, 0, sizeof *p);
}

void math_precision_break(MathPrecision *p)
{
    p->fill = 0;
}

MathPrecisionStep math_precision_batch(MathPrecision *p, const MathDisplay *d, bool timed_out)
{
    if (p->fill < d->prec_window) {
        p->fill++;
    }
    if (p->fill >= d->prec_window
        && d->win.count >= d->prec_window
        && math_window_floor_ready(&d->win)) {
        if (math_window_clean(&d->win, d->prec_window, d->k)) {
            p->disturbed = false;
            for (uint8_t s = 0; s < 2U; ++s) {
                p->result[s] = math_window_hann_mean(&d->win, s, d->prec_window,
                                                     d->hann_prec, d->hann_prec_sum);
            }
            p->failed = false;
            return MATH_PRECISION_OK;
        }
        p->disturbed = true;
    }
    if (timed_out) {
        p->result[0] = p->result[1] = 0.0f;
        p->failed = true;
        return MATH_PRECISION_TIMEOUT;
    }
    return MATH_PRECISION_WAITING;
}
