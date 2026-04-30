#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "stm32g0xx_hal.h"

/* SPI1 — Display (Sharp LS027B7DH01) */
#define DISP_SCK_PORT       GPIOA
#define DISP_SCK_PIN        GPIO_PIN_5      /* PA5, SPI1_SCK AF0 */
#define DISP_MOSI_PORT      GPIOA
#define DISP_MOSI_PIN       GPIO_PIN_7      /* PA7, SPI1_MOSI AF0 */
#define DISP_CS_PORT        GPIOA
#define DISP_CS_PIN         GPIO_PIN_4      /* PA4, active HIGH */
#define DISP_ON_PORT        GPIOA
#define DISP_ON_PIN         GPIO_PIN_8      /* PA8, high = display on */
#define DISP_VCOM_PORT      GPIOA
#define DISP_VCOM_PIN       GPIO_PIN_6      /* PA6, TIM3_CH1 AF1, 30 Hz */

/* Power */
#define LDO_EN_PORT         GPIOC
#define LDO_EN_PIN          GPIO_PIN_0      /* PC0, active high */

/* LEDs */
#define LED_PWR_PORT        GPIOC
#define LED_PWR_PIN         GPIO_PIN_1      /* PC1, green */
#define LED_STS_PORT        GPIOC
#define LED_STS_PIN         GPIO_PIN_2      /* PC2, blue */

/* Crystal: PF0 OSC_IN, PF1 OSC_OUT — configured by CubeMX */
/* SWD: PA13 SWDIO, PA14 SWCLK, PF2 NRST — configured by CubeMX */

/* Reserved for later work packages:
 *   SCL3300 SPI/CS         — WPx
 *   PCAP04 ×2 I2C/INT/RST  — WPx
 *   RN4871 UART            — WPx
 *   24LC256 EEPROM I2C     — WPx
 *   LM35 ADC               — WPx
 *   Encoders               — WPx
 *   Buzzer TIM1            — WPx
 *   USB DP/DM              — WPx
 *   BOOT0                  — WPx
 */

#endif /* PIN_CONFIG_H */
