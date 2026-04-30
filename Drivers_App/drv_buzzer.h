#ifndef DRV_BUZZER_H
#define DRV_BUZZER_H

#include <stdint.h>
#include "drv_common.h"

DrvStatus drv_buzzer_init(void);
void      drv_buzzer_beep(uint16_t freq_hz, uint16_t duration_ms);
void      drv_buzzer_stop(void);

#endif /* DRV_BUZZER_H */
