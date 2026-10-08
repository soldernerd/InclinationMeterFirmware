#ifndef HOST_STM32G0XX_HAL_H
#define HOST_STM32G0XX_HAL_H

/* Host stand-in for the ST HAL umbrella header, just enough for Config/pin_config.h and HAL_App/hal_gpio.h to compile on a
 * PC: the GPIO ports are distinct addresses, the pins single-bit masks. Used only by the tests that include a Services/
 * source directly (tests/test_svc_battery.c) with the hardware layer replaced by test doubles. */

#include <stdint.h>

typedef struct { int id; } GPIO_TypeDef;
extern GPIO_TypeDef g_host_gpio[6];

#define GPIOA (&g_host_gpio[0])
#define GPIOB (&g_host_gpio[1])
#define GPIOC (&g_host_gpio[2])
#define GPIOD (&g_host_gpio[3])
#define GPIOE (&g_host_gpio[4])
#define GPIOF (&g_host_gpio[5])

#define GPIO_PIN_0  ((uint16_t)0x0001U)
#define GPIO_PIN_1  ((uint16_t)0x0002U)
#define GPIO_PIN_2  ((uint16_t)0x0004U)
#define GPIO_PIN_3  ((uint16_t)0x0008U)
#define GPIO_PIN_4  ((uint16_t)0x0010U)
#define GPIO_PIN_5  ((uint16_t)0x0020U)
#define GPIO_PIN_6  ((uint16_t)0x0040U)
#define GPIO_PIN_7  ((uint16_t)0x0080U)
#define GPIO_PIN_8  ((uint16_t)0x0100U)
#define GPIO_PIN_9  ((uint16_t)0x0200U)
#define GPIO_PIN_10 ((uint16_t)0x0400U)
#define GPIO_PIN_11 ((uint16_t)0x0800U)
#define GPIO_PIN_12 ((uint16_t)0x1000U)
#define GPIO_PIN_13 ((uint16_t)0x2000U)
#define GPIO_PIN_14 ((uint16_t)0x4000U)
#define GPIO_PIN_15 ((uint16_t)0x8000U)

/* the peripheral instances pin_config.h names in comments or macros */
typedef struct { int id; } TIM_TypeDef;
extern TIM_TypeDef g_host_tim[8];
#define TIM1 (&g_host_tim[1])
#define TIM2 (&g_host_tim[2])
#define TIM3 (&g_host_tim[3])
#define TIM6 (&g_host_tim[6])
#define TIM7 (&g_host_tim[7])

#endif /* HOST_STM32G0XX_HAL_H */
