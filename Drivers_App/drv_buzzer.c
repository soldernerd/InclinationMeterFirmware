/* WP1 stub — implemented in WPx */
#include "drv_buzzer.h"

DrvStatus drv_buzzer_init(void)                                       { return DRV_OK; }
void      drv_buzzer_beep(uint16_t freq_hz, uint16_t duration_ms)     { (void)freq_hz; (void)duration_ms; }
void      drv_buzzer_stop(void)                                       {}
