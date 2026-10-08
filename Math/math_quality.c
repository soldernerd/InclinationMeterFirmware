#include "math_quality.h"
#include <string.h>

void math_display_reset(MathDisplay *d)
{
    math_window_reset(&d->win, d->creep, d->reseed_batches);
    d->phase = 0;
    d->out[0] = d->out[1] = 0.0f;
    d->doubtful[0] = d->doubtful[1] = false;
    d->valid = false;
    d->have_seq = false;
    d->seq++;
}

void math_display_init(MathDisplay *d, uint16_t taps, uint16_t decimation, uint16_t prec_window,
                       float k, float creep, uint32_t reseed_batches)
{
    memset(d, 0, sizeof *d);
    d->taps           = taps;
    d->decimation     = decimation;
    d->prec_window    = prec_window;
    d->k              = k;
    d->creep          = creep;
    d->reseed_batches = reseed_batches;
    d->hann_disp_sum  = math_hann_weights(d->hann_disp, taps);
    d->hann_prec_sum  = math_hann_weights(d->hann_prec, prec_window);
    math_window_reset(&d->win, creep, reseed_batches);   /* seq stays 0 until the first reset/publish */
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
    }
    d->last_seq = seq;
    d->have_seq = true;

    math_window_push(&d->win, delta, residual);

    if (++d->phase < d->decimation) {
        return contiguous;
    }
    d->phase = 0;
    if (d->win.count < d->taps) {
        return contiguous;   /* window not yet full */
    }
    for (uint8_t s = 0; s < 2U; ++s) {
        d->out[s]      = math_window_hann_mean(&d->win, s, d->taps, d->hann_disp, d->hann_disp_sum);
        d->doubtful[s] = !math_window_clean_sensor(&d->win, s, d->taps, d->k);
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
