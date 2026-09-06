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

/* Power
 *
 * KNOWN HARDWARE MISTAKE (confirmed against the schematic, 2026-08-17):
 * neither of these two pins has a board-level pull resistor, but both
 * drive circuits that explicitly must not be left floating:
 *   - PWR_3V3_EN (PC6) drives a P-MOSFET gate DIRECTLY (no external
 *     pull-up). A floating P-MOSFET gate is an undefined/partially-on
 *     state, not a safe "off".
 *   - PWR_5V_EN (PC7) feeds a regulator's "Active Low Shutdown Input" (no
 *     external pull-down); that chip's own datasheet says "this pin must
 *     not be allowed to float."
 * This matters because STM32 Standby mode powers down the whole GPIO
 * configuration domain — once Standby actually engages, these pins stop
 * being actively driven regardless of what level firmware last set, and
 * with no board-level pull, they float. Mitigated in firmware via STM32's
 * Standby-mode I/O retention (PWR_PUCRx/PDCRx + APC) —
 * HAL_App/hal_power.c's hal_power_configure_rail_retention() holds PC6
 * pulled up (MOSFET off) and PC7 pulled down (shutdown asserted) using
 * the MCU's own weak internal pulls, which — unlike normal GPIO output
 * drive — persist through Standby. Not a substitute for fixing this on
 * the next hardware rev with real pull resistors, but sufficient given
 * REV B boards already exist.
 */
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

/* RN4871 BLE module (WP4). UART is USART6 (PB8 TX / PB9 RX, handled by
 * HAL_App/hal_uart.c). ~BLE_RESET is the only control line to the MCU:
 * PB5, active LOW (low = held in reset, high = run). No MODE/P2_0 pin
 * to the MCU — the module self-selects Application mode. The four
 * BLE_P1_x pins (PB4/PB15/PA9/PA8) are wired but of TBD function; unused
 * for now (drv_rn4871.c tracks connection state from the module's
 * %CONNECT%/%DISCONNECT% UART messages instead). */
#define BLE_RESET_PORT      GPIOB
#define BLE_RESET_PIN       GPIO_PIN_5

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
                                              * driver — see the LM35 formula
                                              * note below (DeviceSettings'
                                              * lm35_scale_mv_per_c) for the
                                              * intended conversion once that
                                              * driver is written. */

/* Battery monitoring — all four signals talk to the TP4056 charger IC
 * (NanJing Top Power TP4056, local datasheet:
 * .../InclinationMeter/_archive/Datasheets/TP4056.pdf).
 * CHARGE_SENSE and STANDBY_SENSE are TP4056 *outputs* (things we read);
 * CHARGE_EN is our *output* into the TP4056's CE input (things we drive).
 * VBUS_SENSE is independent of the TP4056 — it's our own USB-present
 * detector and the thing that makes charging possible at all.
 *
 * IMPORTANT: the TP4056 itself is powered from VBUS (its VCC pin, datasheet
 * 4.0-8.0V range), not from a rail that's up independent of USB. When
 * VBUS_SENSE is LOW, the TP4056 has no power and CHARGE_SENSE/
 * STANDBY_SENSE are undriven/meaningless — svc_battery.c forces both to
 * "not asserted" in that case rather than trusting whatever they float to,
 * it does not just read the pins directly. */
#define CHARGE_SENSE_PORT   GPIOC
#define CHARGE_SENSE_PIN    GPIO_PIN_2      /* PC2, TP4056 CHRG output
                                              * (open-drain, external pull-up),
                                              * active LOW (was PA9). Per
                                              * datasheet: pulled low while
                                              * actively charging, high-Z
                                              * (reads high through the
                                              * pull-up) once terminated. Only
                                              * meaningful while VBUS_SENSE is
                                              * present — see note above. */
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
#define STANDBY_SENSE_PIN   GPIO_PIN_5      /* PD5, TP4056 STANDBY output
                                              * (open-drain, external pull-up),
                                              * active LOW. New — no REV-A
                                              * equivalent. Per datasheet:
                                              * pulled low once the charge
                                              * cycle terminates (C/10 current
                                              * after reaching the 4.2V float
                                              * voltage), high-Z otherwise.
                                              * Used instead of an SOC-percent
                                              * heuristic to detect
                                              * BATTERY_FULL, and to turn
                                              * CHARGE_EN back off
                                              * (svc_battery.c). Only
                                              * meaningful while VBUS_SENSE is
                                              * present — see note above. */
#define CHARGE_EN_PORT      GPIOD
#define CHARGE_EN_PIN       GPIO_PIN_6      /* PD6, !CHARGE_EN!, active LOW
                                              * (Low = charge, High = don't
                                              * charge) — our *output* into the
                                              * TP4056's CE input, via a level-
                                              * translation buffer (that's why
                                              * the polarity is inverted versus
                                              * a direct connection; the
                                              * TP4056's own CE pin is active
                                              * HIGH = normal operation, per
                                              * datasheet). Policy
                                              * (svc_battery.c), to avoid
                                              * keeping an already-near-full
                                              * LiPo topped off (degrades
                                              * battery life over time):
                                              * enable only once Vbat has
                                              * actually dropped to
                                              * battery_low_mv, keep enabled
                                              * (latched — don't oscillate as
                                              * Vbat rises back above that
                                              * threshold mid-charge) until
                                              * STANDBY_SENSE reports complete
                                              * or VBUS disappears, whichever
                                              * comes first. New — no REV-A
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

/* Encoder A/B quadrature signals (WP3, REV B pinout — old REV A prototype
 * had these on PA0-PA3/PC4/PC5, all reassigned; see
 * docs/pinout_migration_wp2-5.md). RC-filtered + 74HC14 Schmitt-trigger
 * *buffered* (NOT inverted, unlike ENC_1SW/ENC_2SW above — confirmed
 * CMOS-driven, no pull needed). Both edges of both A and B are EXTI-driven
 * (GPIO_MODE_IT_RISING_FALLING, Core/Src/gpio.c) so Drivers_App/
 * drv_encoder.c does a full 4x quadrature decode rather than the REV A
 * prototype's simpler A-edge-only approach. */
#define ENC_1A_PORT         GPIOC
#define ENC_1A_PIN          GPIO_PIN_4      /* PC4, EXTI4 */
#define ENC_1B_PORT         GPIOB
#define ENC_1B_PIN          GPIO_PIN_0      /* PB0, EXTI0 */
#define ENC_2A_PORT         GPIOB
#define ENC_2A_PIN          GPIO_PIN_1      /* PB1, EXTI1 */
#define ENC_2B_PORT         GPIOB
#define ENC_2B_PIN          GPIO_PIN_2      /* PB2, EXTI2 */

/* Buzzer (WP3): Same Sky CPT-9019A-SMT-TR piezo transducer, externally
 * driven (no internal oscillator) — needs a continuous drive square wave,
 * not just a logic level. Datasheet's frequency-response curve peaks
 * loudest around ~5.5 kHz (~84 dB); driven at ~2 kHz instead (~73 dB, a
 * secondary response peak) per user preference — less piercing. Moved
 * from the REV A prototype's TIM1_CH2/PB3 to TIM3_CH4/PC9 on REV B.
 * HAL_App/hal_tim.c drives it via PWM, prescaler 63 -> 1 MHz tick
 * (confirmed APB1 = HCLK = 64 MHz, no prescaler, in
 * SystemClock_Config()), ARR = (1,000,000 / freq_hz) - 1. Needs 5V_EN
 * (see PWR_5V_EN above) — already asserted unconditionally at boot. Not a
 * plain-GPIO macro here since it's always driven via the TIM3 AF path
 * (Core/Src/tim.c's HAL_TIM_MspPostInit); listed for documentation only. */
#define BUZZER_PORT         GPIOC
#define BUZZER_PIN          GPIO_PIN_9      /* PC9, TIM3_CH4 (AF1) */

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
 * HAL_App/hal_adc.h), not a raw-code shortcut — REV B ties VREF+ directly
 * to the 3V3_STANDBY rail, not a fixed-voltage VREFBUF, so a constant-
 * reference assumption is wrong on this board.
 *
 * KNOWN HARDWARE MISTAKE (2026-08-17, acknowledged, not being reworked):
 * this divider is backwards from optimal. With the high side (100k) on top
 * and low side (33k) on bottom, a full 4.2V battery only reaches
 * V_ADC = 4200x33/133 ~ 1042 mV — using under a third of the ADC's 0-VDDA
 * (~3.3V) range. Swapped (33k top / 100k bottom), the same 4.2V would
 * reach V_ADC = 4200x100/133 ~ 3158 mV, using nearly the full range for
 * much better resolution. No safety issue either way (well under VDDA,
 * no overvoltage risk) and the resolution we do get is still adequate —
 * roughly 3.3 mV of Vbat per ADC LSB (vs. ~1.1 mV/LSB if swapped) with
 * 12-bit + 16x oversampled ADC1, plenty fine for SOC estimation — so this
 * is a "note for next hardware rev" rather than something worth reworking
 * on already-built boards.
 *
 * The actual scale factor consumed by the conversion math
 * (g_device_settings.vbat_scale_num/den) is EEPROM-backed, not a #define
 * here — see config.h's DEFAULT_VBAT_SCALE_NUM/DEN for the seed value and
 * system_state.h for the field. VBAT_DIV_HIGH_K/LOW_K below are pure BOM
 * documentation of the physical resistors, not consumed by any code. */
#define VBAT_DIV_HIGH_K     100
#define VBAT_DIV_LOW_K      33

/* LM35 (for the future TEMP_SENSE_EXT driver) and TMP236 (TEMP_SENSE,
 * Drivers_App/drv_tmp236.c) conversion formulas are also EEPROM-backed —
 * see config.h's DEFAULT_LM35_SCALE_MV_PER_C / DEFAULT_TMP236_* for the
 * seed values and system_state.h for the fields. No calibration constant
 * lives only in flash (project rule, 2026-08-17).
 *
 * LM35 formula, verified against the actual datasheet (TI SNIS159H,
 * "LM35 Precision Centigrade Temperature Sensors"; local copy:
 * .../InclinationMeter/_archive/Datasheets/LM35.pdf), not assumed from
 * memory: VOUT = 10 mV/°C x T, 0 mV at 0°C, single linear equation (no
 * piecewise segments like TMP236 — plain LM35 doesn't need one).
 * Supply: datasheet specifies 4V-30V — our 5V rail is comfortably inside
 * that range (confirms why TEMP_SENSE_EXT needs 5V_EN, not just 3.3V).
 * CAVEAT — negative temperatures: the basic single-supply hookup (VOUT
 * straight into an ADC pin, no negative bias) cannot output a negative
 * voltage, so it can't indicate temperatures below ~0°C at all in that
 * configuration — the datasheet's full -55°C to +150°C range needs an
 * extra pull-down resistor to a small negative bias (its "Figure 18"
 * circuit). NOT verified against the actual REV B schematic whether that
 * bias network exists around TEMP_SENSE_EXT or whether it's a bare
 * single-supply hookup — check before assuming this sensor can read
 * freezing/sub-zero temperatures once its driver is written. */

/* VREFINT factory calibration: use stm32g0xx_ll_adc.h's own
 * VREFINT_CAL_ADDR/VREFINT_CAL_VREF (same address/value) — do not redefine
 * here, it collides with the LL driver's macro of the same name. */

/* Crystal: PF0 OSC_IN, PF1 OSC_OUT, 8 MHz HSE — configured by CubeMX */
/* SWD: PA13 SWDIO, PA14 SWCLK — configured by CubeMX. NRST is the dedicated
 * MCU reset pin, not a GPIO. */

/* SPI3 — AD9833 waveform generator DAC (WP7). AD9833BRMZ-REEL7. Write-only:
 * only MOSI is wired, no MISO — matches the chip's own 3-wire
 * (SCLK / SDATA / FSYNC) serial interface, which has no return data path. */
#define AD9833_SCK_PORT      GPIOC
#define AD9833_SCK_PIN       GPIO_PIN_10     /* PC10, SPI3_SCK */
#define AD9833_MOSI_PORT     GPIOC
#define AD9833_MOSI_PIN      GPIO_PIN_12     /* PC12, SPI3_MOSI -> AD9833 SDATA */
#define AD9833_FSYNC_PORT    GPIOC
#define AD9833_FSYNC_PIN     GPIO_PIN_13     /* PC13, plain GPIO, active LOW — the
                                               * AD9833's FSYNC (frame sync) is a
                                               * level-triggered input, NOT SPI3_NSS.
                                               * Same software-CS pattern as the
                                               * display's DISP_CS; SPI3 is
                                               * SPI_NSS_SOFT. Idles HIGH. */
#define AD9833_CLOCK_PORT    GPIOC
#define AD9833_CLOCK_PIN     GPIO_PIN_11     /* PC11, TIM1_CH4 PWM — the DAC's MCLK
                                               * feed (5.333 MHz, see
                                               * hal_tim_dac_clock_start()); a
                                               * separate signal from SPI3_SCK
                                               * above despite the name. */

/* SPI1 — ADS131M04 simultaneous-sampling ADC (WP8). ADS131M04IPWR. True
 * full-duplex: DIN and DOUT are both used — every frame sends a command
 * and receives the previous frame's response + four 24-bit conversions. */
#define ADC_SCK_PORT         GPIOA
#define ADC_SCK_PIN          GPIO_PIN_5      /* PA5, SPI1_SCK */
#define ADC_MISO_PORT        GPIOA
#define ADC_MISO_PIN         GPIO_PIN_6      /* PA6, SPI1_MISO <- ADS131M04 DOUT */
#define ADC_MOSI_PORT        GPIOA
#define ADC_MOSI_PIN         GPIO_PIN_7      /* PA7, SPI1_MOSI -> ADS131M04 DIN */
#define ADC_CS_PORT          GPIOA
#define ADC_CS_PIN           GPIO_PIN_4      /* PA4, active LOW — plain GPIO, NOT
                                               * SPI1_NSS. Software CS (SPI1 is
                                               * SPI_NSS_SOFT): held asserted
                                               * across the whole multi-word
                                               * frame. */
#define ADC_SYNC_RESET_PORT  GPIOC
#define ADC_SYNC_RESET_PIN   GPIO_PIN_3      /* PC3, plain GPIO Output, active LOW
                                               * — dual-function SYNC/RESET input.
                                               * Idle HIGH; pulsed low >=2048 CLKIN
                                               * cycles at init (datasheet
                                               * t_w(RSL)). */
#define ADC_READY_PORT       GPIOA
#define ADC_READY_PIN        GPIO_PIN_1      /* PA1, plain GPIO Input, active LOW —
                                               * ADS131M04 DRDY. NOT EXTI: PB1
                                               * (ENC_2A) already owns EXTI1. The
                                               * DAC->ADC chain is fully
                                               * deterministic (shared 64 MHz via
                                               * fixed prescalers) so
                                               * drv_ads131m04.c polls this level
                                               * from the TIM7 sample-rate ISR
                                               * instead of reacting to an edge. */
#define ADC_CLOCK_PORT       GPIOB
#define ADC_CLOCK_PIN        GPIO_PIN_10     /* PB10, TIM2_CH3 PWM — the ADC's MCLK
                                               * feed (5.333 MHz,
                                               * hal_tim_adc_clock_start()), not
                                               * the SPI clock. Same 64 MHz/(2x6)
                                               * generation as the DAC's
                                               * TIM1_CH4/PC11. */

/* Reserved for later work packages:
 *   SCL3300 / PCAP04 — removed from REV B hardware, no longer applicable
 *
 * Encoder A/B quadrature and buzzer TIM3_CH4 were WP3 scope, USB DP/DM
 * was WP4, RN4871 UART/reset/GPIO was WP5, AD9833 DAC was WP7, ADS131M04
 * ADC above is WP8 — all now implemented. */

#endif /* PIN_CONFIG_H */
