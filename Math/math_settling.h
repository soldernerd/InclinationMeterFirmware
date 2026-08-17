#ifndef MATH_SETTLING_H
#define MATH_SETTLING_H

#include <stdint.h>

typedef enum {
    SETTLING_COLLECTING = 0,
    SETTLING_SETTLED,
    SETTLING_TIMEOUT,
} SettlingState;

typedef struct {
    int32_t  dc_umpm;          /* result — simple mean in V1 */
    int32_t  amplitude_umpm;   /* 0 in V1 (not computed) */
    uint16_t outlier_count;    /* 0 in V1 */
    uint16_t sample_count;     /* samples collected so far */
} SettlingResult;

/* Caller owns `buffer` (length = `buffer_size`). svc_measurement holds a
 * persistent buffer + state across update() calls. The function pushes
 * `new_sample` and recomputes mean/variance every call. */
SettlingState math_settling_update(int32_t new_sample,
                                   int32_t *buffer,
                                   uint16_t buffer_size,
                                   uint16_t *count_inout,
                                   uint16_t *write_idx_inout,
                                   int32_t threshold_umpm,
                                   uint32_t timeout_ms,
                                   uint32_t elapsed_ms,
                                   SettlingResult *result);

void math_settling_reset(uint16_t *count_inout, uint16_t *write_idx_inout);

#endif /* MATH_SETTLING_H */
