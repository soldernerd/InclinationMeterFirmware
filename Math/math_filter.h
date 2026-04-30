#ifndef MATH_FILTER_H
#define MATH_FILTER_H

#include <stdint.h>

void    math_filter_reset(void);
int32_t math_filter_step(int32_t sample);

#endif /* MATH_FILTER_H */
