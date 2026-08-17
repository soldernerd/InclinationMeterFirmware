#include "math_settling.h"

void math_settling_reset(uint16_t *count_inout, uint16_t *write_idx_inout)
{
    if (count_inout)     *count_inout     = 0;
    if (write_idx_inout) *write_idx_inout = 0;
}

SettlingState math_settling_update(int32_t new_sample,
                                   int32_t *buffer,
                                   uint16_t buffer_size,
                                   uint16_t *count_inout,
                                   uint16_t *write_idx_inout,
                                   int32_t threshold_umpm,
                                   uint32_t timeout_ms,
                                   uint32_t elapsed_ms,
                                   SettlingResult *result)
{
    if (buffer == 0 || count_inout == 0 || write_idx_inout == 0
        || result == 0 || buffer_size == 0) {
        return SETTLING_TIMEOUT;
    }

    /* Push the sample into the circular buffer */
    buffer[*write_idx_inout] = new_sample;
    *write_idx_inout = (uint16_t)((*write_idx_inout + 1U) % buffer_size);
    if (*count_inout < buffer_size) {
        (*count_inout)++;
    }

    uint16_t n = *count_inout;
    result->sample_count   = n;
    result->amplitude_umpm = 0;
    result->outlier_count  = 0;

    if (n == 0) {
        result->dc_umpm = 0;
        return SETTLING_COLLECTING;
    }

    /* Mean — int64 accumulator covers 256 samples x 2^31 worst case */
    int64_t sum = 0;
    for (uint16_t i = 0; i < n; ++i) {
        sum += buffer[i];
    }
    int32_t mean = (int32_t)(sum / (int64_t)n);
    result->dc_umpm = mean;

    /* Variance — sum of squared deviations / n */
    int64_t sum_sq = 0;
    for (uint16_t i = 0; i < n; ++i) {
        int64_t dev = (int64_t)buffer[i] - (int64_t)mean;
        sum_sq += dev * dev;
    }
    int64_t variance = sum_sq / (int64_t)n;

    /* Settled if variance < threshold^2 (positive int64 multiply, no
     * overflow for sane thresholds — threshold_umpm = 10 yields 100). */
    int64_t thresh_sq = (int64_t)threshold_umpm * (int64_t)threshold_umpm;

    if (n >= 8U && variance < thresh_sq) {
        return SETTLING_SETTLED;
    }
    if (elapsed_ms >= timeout_ms) {
        return SETTLING_TIMEOUT;
    }
    return SETTLING_COLLECTING;
}
