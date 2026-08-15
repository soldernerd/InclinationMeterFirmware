#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "stm32g0xx_hal.h"

/* ============================================================================
 * REV B pinout (STM32G0B1RET6_Pinout.csv). See docs/pinout_migration_wp2-5.md
 * and docs/cubemx_configuration_checklist.md for the full migration record.
 * ============================================================================
 */

/* SPI2 — Display (Sharp LS027B7DH01) */
#define DISP_SCK_PORT       GPIOD
#define DISP_SCK_PIN        GPIO_PIN_1      /* PD1, SPI2_SCK */
#define DISP_MOSI_PORT      GPIOD
#define DISP_MOSI_PIN       GPIO_PIN_4      /* PD4, SPI2_MOSI */
#define DISP_CS_PORT        GPIOD
#define DISP_CS_PIN         GPIO_PIN_0      /* PD0, active HIGH — plain GPIO, NOT
                                              * SPI2_NSS. This SPI IP's hardware NSS
                                              * is fixed active-low with no polarity
                                              * bit, incompatible with the display's
                                              * active-HIGH CS — must stay software
                                              * GPIO (SPI2 is SPI_NSS_SOFT). */
#define DISP_ON_PORT        GPIOD
#define DISP_ON_PIN         GPIO_PIN_2      /* PD2, high = display on */
#define DISP_VCOM_PORT      GPIOD
#define DISP_VCOM_PIN       GPIO_PIN_3      /* PD3, plain GPIO, toggled by TIM6 ISR
                                              * at 5 Hz (datasheet: 1-10 Hz) — no
                                              * hardware PWM channel on REV B. */

/* Power */
#define PWR_3V3_EN_PORT     GPIOC
#define PWR_3V3_EN_PIN      GPIO_PIN_6      /* PC6, !3V3_EN!, active LOW (Low = on,
                                              * High = off) — inverted vs. the old
                                              * active-high LDO_EN. Also asserted
                                              * pre-HAL_Init() in main.c (LL calls) —
                                              * see that file for why. */
#define PWR_5V_EN_PORT      GPIOC
#define PWR_5V_EN_PIN       GPIO_PIN_7      /* PC7, 5V_EN, active HIGH — display,
                                              * buzzer, and both temp sensors need
                                              * this rail. */

/* LEDs */
#define LED_PWR_PORT        GPIOB
#define LED_PWR_PIN         GPIO_PIN_13     /* PB13, active HIGH */
#define LED_STS_PORT        GPIOB
#define LED_STS_PIN         GPIO_PIN_14     /* PB14, active HIGH */

/* ADC1 — internal battery/temperature sensing.
 * NOT_FULLY_CONFIGURABLE sequencer: hardware always converts in ascending
 * channel-number order regardless of scan-list order, so the real/fixed
 * DMA scan order is CH3, CH13(VREFINT), CH15, CH16 — see hal_adc.c. */
#define BATTERY_SENSE_PORT  GPIOA
#define BATTERY_SENSE_PIN   GPIO_PIN_3      /* PA3, ADC1_IN3 (was PB0/IN8).
                                              * Needs the 3.3V rail
                                              * (PWR_3V3_EN above) — unlike
                                              * the temp sensors this is the
                                              * always-on switched rail, so
                                              * no settle-delay gating is
                                              * needed beyond boot's existing
                                              * one (see hal_gpio_init()). */
#define TEMP_SENSE_PORT     GPIOB
#define TEMP_SENSE_PIN      GPIO_PIN_12     /* PB12, ADC1_IN16 (was PB1/IN9).
                                              * On-board sensor is a TI TMP236
                                              * (not an LM35 — REV B changed
                                              * parts). Powered from the 5V
                                              * rail (PWR_5V_EN above), same as
                                              * TEMP_SENSE_EXT — do not read
                                              * before that rail is up and
                                              * settled. Driven by
                                              * Drivers_App/drv_tmp236.c, which
                                              * implements TI's published
                                              * piecewise-linear transfer
                                              * function (datasheet SBOS857E). */
#define TEMP_SENSE_EXT_PORT GPIOB
#define TEMP_SENSE_EXT_PIN  GPIO_PIN_11     /* PB11, ADC1_IN15 — new external
                                              * temperature sensor input, no
                                              * REV-A equivalent. Expected to
                                              * be an LM35 (10 mV/°C, 0 V at
                                              * 0°C) — also powered from the 5V
                                              * rail (PWR_5V_EN above; classic
                                              * LM35 needs >=4V, won't run off
                                              * 3.3V). Not yet consumed by any
                                              * driver — see LM35_SCALE below
                                              * for the intended conversion
                                              * once that driver is written. */

/* Battery monitoring — all four signals talk to the TP4056 charger IC.
 * CHARGE_SENSE and STANDBY_SENSE are TP4056 *outputs* (things we read);
 * CHARGE_EN is our *output* into the TP4056's CE input (things we drive).
 * VBUS_SENSE is independent of the TP4056 — it's our own USB-present
 * detector and the thing that makes charging possible at all. */
#define CHARGE_SENSE_PORT   GPIOC
#define CHARGE_SENSE_PIN    GPIO_PIN_2      /* PC2, TP4056 CHRG output, active
                                              * LOW, pull-up (was PA9). LOW =
                                              * actively charging right now. */
#define VBUS_SENSE_PORT     GPIOA
#define VBUS_SENSE_PIN      GPIO_PIN_2      /* PA2, USB VBUS detect, active HIGH
                                              * (was PA10) — our own power-input-
                                              * present signal, not a TP4056
                                              * pin. Without this HIGH we have no
                                              * charge source, regardless of
                                              * CHARGE_EN. Configured as
                                              * SYS_WKUP4 in CubeMX with no
                                              * GPIO_Init call — hal_gpio_init()
                                              * re-configures it as a digital
                                              * input so reads work in Run mode;
                                              * see hal_gpio.c. Also one of the
                                              * three Standby wake-up sources —
                                              * see the "Low-power / Standby"
                                              * block below. */
#define STANDBY_SENSE_PORT  GPIOD
#define STANDBY_SENSE_PIN   GPIO_PIN_5      /* PD5, TP4056 STANDBY output,
                                              * active LOW, pull-up. New — no
                                              * REV-A equivalent. LOW = charge
                                              * cycle complete (battery full);
                                              * used instead of an SOC-percent
                                              * heuristic to detect BATTERY_FULL
                                              * (svc_battery.c). */
#define CHARGE_EN_PORT      GPIOD
#define CHARGE_EN_PIN       GPIO_PIN_6      /* PD6, !CHARGE_EN!, active LOW
                                              * (Low = charge, High = don't
                                              * charge) — our *output* into the
                                              * TP4056's CE input, via a level-
                                              * translation buffer (that's why
                                              * the polarity is inverted versus
                                              * a direct connection). Policy
                                              * (svc_battery.c): enable
                                              * whenever VBUS_SENSE is present,
                                              * disable otherwise — the TP4056
                                              * itself autonomously stops
                                              * actively charging once full
                                              * (reflected in STANDBY_SENSE),
                                              * we don't need to toggle CE for
                                              * that. New — no REV-A
                                              * equivalent, old hardware had no
                                              * charge-enable control, only CHG
                                              * sense. */

/* Low-power / Standby wake-up pins.
 *
 * Design (svc_battery.c owns the decision, HAL_App/hal_power.c wraps the
 * ST HAL PWR calls):
 *   - The MCU's own supply (3V3_STANDBY) is a separate, always-on rail from
 *     PWR_3V3_EN/PWR_5V_EN — the MCU itself is never powered off. "Low
 *     power" here means putting the MCU into STM32 Standby mode
 *     (~0.28 uA, CLAUDE.md §8.4) after first disabling both switched
 *     rails (PWR_3V3_EN, PWR_5V_EN) and both LEDs.
 *   - Entry trigger (current, WP2): battery reaches BATTERY_CRITICAL AND
 *     VBUS_SENSE is not present (if VBUS is present we charge instead of
 *     shutting down — see CHARGE_EN above). A later WP may also trigger
 *     entry from a user-initiated power-off.
 *   - Standby mode resets the MCU on wake (all RAM/state is lost) —
 *     execution resumes at the reset vector, same as a power-on reset.
 *     hal_gpio_init()'s existing boot sequence already re-enables both
 *     rails, so "waking up" needs no special re-init code beyond a normal
 *     boot; svc_battery_update() re-evaluates battery/USB state fresh on
 *     every boot regardless of how it started.
 *   - Wake sources (all high-level detection — see ENC_1SW/ENC_2SW below
 *     for why "active-low" mechanical switches produce a HIGH wake
 *     signal): ENC_1SW (WKUP1), VBUS_SENSE (WKUP4), ENC_2SW (WKUP5). If
 *     wake was caused by VBUS_SENSE and the battery is still critical,
 *     we do NOT re-enter low power — the !s_usb_connected gate on the
 *     shutdown trigger already ensures this without any extra "how did
 *     we wake up" bookkeeping.
 */
#define ENC_1SW_PORT        GPIOA
#define ENC_1SW_PIN         GPIO_PIN_0      /* PA0, SYS_WKUP1. Encoder 1
                                              * push switch. The raw
                                              * mechanical switch is active-
                                              * LOW (CLAUDE.md §5.11), but it
                                              * passes through an RC filter +
                                              * 74HC14 Schmitt-trigger
                                              * *inverter* before reaching the
                                              * MCU (same §5.11 hardware) — so
                                              * the pin the MCU/PWR peripheral
                                              * actually sees goes HIGH on
                                              * press. Defined here only for
                                              * its WKUP1 role; full encoder
                                              * A/B quadrature reading is
                                              * WP3 scope. */
#define ENC_2SW_PORT        GPIOC
#define ENC_2SW_PIN         GPIO_PIN_5      /* PC5, SYS_WKUP5. Same as
                                              * ENC_1SW above (encoder 2's
                                              * push switch). */

/* I2C1 — EEPROM (and BME280 in future WPs) */
#define I2C1_SCL_PORT       GPIOB
#define I2C1_SCL_PIN        GPIO_PIN_6      /* PB6, I2C1_SCL AF6 — unchanged */
#define I2C1_SDA_PORT       GPIOB
#define I2C1_SDA_PIN        GPIO_PIN_7      /* PB7, I2C1_SDA AF6 — unchanged */

/* Vbat divider (confirmed against REV B schematic, 2026-08-17 — corrects
 * an earlier 100k/68k assumption carried over from REV A):
 *   R_VBAT1 = 100 kΩ  (high side)
 *   R_VBAT2 =  33 kΩ  (low side)
 *   V_ADC   = Vbat × 33 / 133
 *   Vbat_mv = V_ADC_mv × 133 / 33
 * V_ADC_mv must come from hal_adc_raw_to_mv() (VREFINT-ratiometric — see
 * HAL_App/hal_adc.h), not a raw-code shortcut; see the LM35_SCALE comment
 * below for why a fixed-reference assumption is wrong on this board. */
#define VBAT_DIV_HIGH_K     100
#define VBAT_DIV_LOW_K      33
#define VBAT_SCALE_NUM      133
#define VBAT_SCALE_DEN      33

/* LM35 conversion for the future TEMP_SENSE_EXT driver: 10 mV/°C, 0 V at
 * 0°C, no offset. Use hal_adc_raw_to_mv() (HAL_App/hal_adc.h) to get actual
 * millivolts first — REV B ties VREF+ directly to the 3V3_STANDBY rail, not
 * a fixed-voltage VREFBUF, so a raw-code shortcut assuming a constant
 * reference (as this file used to have) is wrong. Once converted to mV:
 *   Temp_cdeg = V_mV × 10
 * Not yet consumed by any driver — TEMP_SENSE_EXT has no driver yet (see
 * TEMP_SENSE_EXT_PIN above). LM35_SCALE kept as documentation of the
 * intended formula, not currently referenced by code. */
#define LM35_SCALE          10

/* VREFINT factory calibration: use stm32g0xx_ll_adc.h's own
 * VREFINT_CAL_ADDR/VREFINT_CAL_VREF (same address/value) — do not redefine
 * here, it collides with the LL driver's macro of the same name. */

/* Crystal: PF0 OSC_IN, PF1 OSC_OUT, 8 MHz HSE — configured by CubeMX */
/* SWD: PA13 SWDIO, PA14 SWCLK — configured by CubeMX. NRST is the dedicated
 * MCU reset pin, not a GPIO. */

/* Reserved for later work packages:
 *   SCL3300 / PCAP04            — removed from REV B hardware, no longer applicable
 *   RN4871 UART                 — WP5 (USART6 now, was USART2)
 *   Encoder A/B quadrature       — WP3 (PC4/PB0/PB1/PB2). Push switches
 *                                  ENC_1SW/ENC_2SW are already defined above
 *                                  for their WKUP1/WKUP5 role.
 *   Buzzer TIM3_CH4              — WP3 (PC9, was TIM1)
 *   USB DP/DM                   — WP4 (PA11/PA12, unchanged, standard pins)
 *   External ADC/DAC front end (SPI1/SPI3) — unidentified, future WP
 */

#endif /* PIN_CONFIG_H */
