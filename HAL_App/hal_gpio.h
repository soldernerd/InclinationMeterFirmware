#ifndef HAL_GPIO_H
#define HAL_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32g0xx_hal.h"

typedef void (*HalGpioExtiCallback)(void);

void hal_gpio_init(void);
void hal_gpio_set(GPIO_TypeDef *port, uint16_t pin, bool state);
bool hal_gpio_get(GPIO_TypeDef *port, uint16_t pin);
void hal_gpio_exti_register(uint8_t pin, HalGpioExtiCallback cb);

#endif /* HAL_GPIO_H */
