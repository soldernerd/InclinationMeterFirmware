#ifndef SVC_INPUT_H
#define SVC_INPUT_H

/* Local UI input (WP3): polls the two encoder push switches (not EXTI-
 * capable on this pinout — see pin_config.h's ENC_1SW/ENC_2SW comments)
 * and mirrors both switches and the EXTI-driven encoder rotation counts
 * (Drivers_App/drv_encoder.c) into g_system_state. Pure input polling —
 * App/app_ui.c is the consumer that decides navigation and buzzer
 * feedback. Call svc_input_update() from the scheduler every tick. */
void svc_input_init(void);
void svc_input_update(void);

#endif /* SVC_INPUT_H */
