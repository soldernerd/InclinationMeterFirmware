#include "math_noise.h"
#include <math.h>
#include <string.h>

void math_noise_reset(MathNoise *n)
{
    for (uint8_t c = 0; c < MATH_NOISE_CHANNELS; ++c) {
        n->sum[c]   = 0.0;
        n->sumsq[c] = 0.0;
        n->ref[c]   = 0.0f;
    }
    /* the ring contents are ignored while count == 0 and overwritten before they are read */
    n->next     = 0;
    n->count    = 0;
    n->have_ref = false;
}

void math_noise_push(MathNoise *n, const float v[MATH_NOISE_CHANNELS])
{
    if (!n->have_ref) {
        for (uint8_t c = 0; c < MATH_NOISE_CHANNELS; ++c) {
            n->ref[c] = v[c];
        }
        n->have_ref = true;
    }
    const bool full = (n->count == MATH_NOISE_LEN);
    for (uint8_t c = 0; c < MATH_NOISE_CHANNELS; ++c) {
        const float x = v[c] - n->ref[c];
        if (full) {
            const double old = (double)n->ring[c][n->next];
            n->sum[c]   -= old;
            n->sumsq[c] -= old * old;
        }
        n->ring[c][n->next] = x;
        n->sum[c]   += (double)x;
        n->sumsq[c] += (double)x * (double)x;
    }
    n->next = (uint16_t)((n->next + 1U) % MATH_NOISE_LEN);
    if (!full) {
        n->count++;
    }
}

void math_noise_stats(const MathNoise *n, MathNoiseStats *out)
{
    out->valid = (n->count == MATH_NOISE_LEN);
    out->count = n->count;
    for (uint8_t c = 0; c < MATH_NOISE_CHANNELS; ++c) {
        out->sigma[c] = 0.0f;
        out->p2p[c]   = 0.0f;
        if (n->count < 2U) {
            continue;
        }
        const double cnt = (double)n->count;
        double var = (n->sumsq[c] - n->sum[c] * n->sum[c] / cnt) / (cnt - 1.0);
        if (var < 0.0) {
            var = 0.0;   /* rounding on a constant signal */
        }
        out->sigma[c] = (float)sqrt(var);
        float lo = n->ring[c][0];
        float hi = lo;
        /* while the window is not full only slots 0..count-1 are written */
        for (uint16_t i = 1; i < n->count; ++i) {
            const float x = n->ring[c][i];
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        out->p2p[c] = hi - lo;
    }
}
