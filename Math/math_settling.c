/* WP1 stub — implemented in WPx */
#include "math_settling.h"

void math_settling_reset(void)                                { }
void math_settling_push(int32_t sample_umpm)                  { (void)sample_umpm; }
bool math_settling_is_settled(int32_t threshold_umpm)         { (void)threshold_umpm; return false; }
