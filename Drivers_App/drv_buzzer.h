#ifndef DRV_BUZZER_H
#define DRV_BUZZER_H

#include <stdint.h>

typedef enum {
    /* Single tone for both encoder-turn and button-press feedback, for
     * now (2026-08-17 user decision) — the piezo's frequency-response
     * curve (Same Sky CPT-9019A-SMT-TR datasheet) actually peaks loudest
     * around ~5.5 kHz, but 2 kHz was chosen as less piercing. Revisit if
     * a future WP wants distinct nav/confirm tones. */
    BUZZER_TONE_CLICK = 2000,
} BuzzerTone;

void drv_buzzer_init(void);
void drv_buzzer_on(BuzzerTone tone);
void drv_buzzer_off(void);
void drv_buzzer_beep(BuzzerTone tone, uint16_t duration_ms);
void drv_buzzer_update(void);   /* call from scheduler every tick */

#endif /* DRV_BUZZER_H */
