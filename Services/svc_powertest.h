#ifndef SVC_POWERTEST_H
#define SVC_POWERTEST_H

#include <stdint.h>
#include <stdbool.h>

/* Runtime power-consumption investigation aid. A single u32 bitmask, set
 * over the API (EXECUTE / Commands / 0x03), where each bit enables one
 * subsystem/rail/clock. Bits that clear are turned OFF immediately; bits
 * that set are turned back ON. UART (USART3) and USB run off the
 * always-on 3V3_STANDBY rail, so every combination — including cutting
 * both switched rails — is still reachable and controllable.
 *
 * Default at boot = PWRTEST_ALL (exactly the normal firmware behaviour),
 * so a baseline measured before touching this still applies.
 *
 * This is a diagnostic, not a low-power mode: turning things back on is
 * best-effort (e.g. BLE needs its module re-init, the display loses its
 * framebuffer). Reboot for a clean state. */

#define PWRTEST_5V_RAIL     (1UL << 0)  /* PWR_5V_EN: display, temp sensors, buzzer buffer, -5V inverter, analog AFE */
#define PWRTEST_3V3_RAIL    (1UL << 1)  /* PWR_3V3_EN: EEPROM, BME280, battery-sense divider */
#define PWRTEST_AD9833     (1UL << 2)  /* DDS: SLEEP the chip + stop its MCLK (TIM1_CH4) */
#define PWRTEST_ADS131M04  (1UL << 3)  /* ADC: stop stream, stop MCLK (TIM2_CH3), hold SYNC/RESET low */
#define PWRTEST_BLE        (1UL << 4)  /* RN4871: hold ~BLE_RESET low */
#define PWRTEST_DISPLAY    (1UL << 5)  /* DISP_ON low + stop the VCOM toggle (TIM6) */
#define PWRTEST_LEDS       (1UL << 6)  /* LED_PWR + LED_STS off */
#define PWRTEST_CPU_SPIN   (1UL << 7)  /* set = scheduler busy-loops (normal); clear = __WFI() between ticks */

#define PWRTEST_ALL   (PWRTEST_5V_RAIL | PWRTEST_3V3_RAIL | PWRTEST_AD9833 | \
                       PWRTEST_ADS131M04 | PWRTEST_BLE | PWRTEST_DISPLAY | \
                       PWRTEST_LEDS | PWRTEST_CPU_SPIN)
#define PWRTEST_KNOWN_BITS  PWRTEST_ALL

void     svc_powertest_init(void);
void     svc_powertest_apply(uint32_t mask);   /* diff vs current, act on changed bits */
uint32_t svc_powertest_mask(void);

/* Cheap accessors polled from hot paths. */
bool     svc_powertest_cpu_spin(void);   /* App/app_scheduler.c */
bool     svc_powertest_leds_on(void);    /* App/app_leds.c */

#endif /* SVC_POWERTEST_H */
