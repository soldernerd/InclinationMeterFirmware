#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "stm32g0xx_hal.h"

/* SPI2 — Display (Sharp LS027B7DH01) */
#define DISP_SCK_PORT       GPIOD
#define DISP_SCK_PIN        GPIO_PIN_1      /* PD1, SPI2_SCK AF1 */
#define DISP_MOSI_PORT      GPIOD
#define DISP_MOSI_PIN       GPIO_PIN_4      /* PD4, SPI2_MOSI AF1 */
#define DISP_CS_PORT        GPIOD
#define DISP_CS_PIN         GPIO_PIN_0      /* PD0, software CS (no hardware-NSS polarity invert on this SPI IP), active HIGH */
#define DISP_ON_PORT        GPIOD
#define DISP_ON_PIN         GPIO_PIN_2      /* PD2, high = display on */
#define DISP_VCOM_PORT      GPIOD
#define DISP_VCOM_PIN       GPIO_PIN_3      /* PD3, plain GPIO, toggled by TIM6 ISR at 5 Hz (datasheet: 1-10 Hz) */

/* Power */
#define PWR_3V3_EN_PORT     GPIOC
#define PWR_3V3_EN_PIN      GPIO_PIN_6      /* PC6, !3V3_EN!, active LOW */
#define PWR_5V_EN_PORT      GPIOC
#define PWR_5V_EN_PIN       GPIO_PIN_7      /* PC7, 5V_EN, active HIGH — display, buzzer, and both temp sensors need this rail */

/* LEDs */
#define LED_PWR_PORT        GPIOB
#define LED_PWR_PIN         GPIO_PIN_13     /* PB13, active HIGH */
#define LED_STS_PORT        GPIOB
#define LED_STS_PIN         GPIO_PIN_14     /* PB14, active HIGH */

/* Crystal: PF0 OSC_IN, PF1 OSC_OUT — configured by CubeMX */
/* SWD: PA13 SWDIO, PA14 SWCLK — configured by CubeMX. NRST is the dedicated
 * MCU reset pin, not a GPIO. */

/* Reserved for later work packages:
 *   SPI1 external ADC front end — WPx
 *   SPI3 external DAC           — WPx
 *   I2C1 EEPROM (BL24C256A) + BME280 — WPx
 *   USART6 RN4871 BLE UART      — WPx
 *   ADC1 battery/temp sensing   — WPx
 *   Encoders                    — WPx
 *   Buzzer TIM3                 — WPx
 *   USB DP/DM                   — WPx
 */

#endif /* PIN_CONFIG_H */
