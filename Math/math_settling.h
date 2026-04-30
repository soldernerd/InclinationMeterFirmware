#ifndef MATH_SETTLING_H
#define MATH_SETTLING_H

#include <stdint.h>
#include <stdbool.h>

void math_settling_reset(void);
void math_settling_push(int32_t sample_umpm);
bool math_settling_is_settled(int32_t threshold_umpm);

#endif /* MATH_SETTLING_H */
