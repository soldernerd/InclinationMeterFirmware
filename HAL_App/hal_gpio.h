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

/* Converts a one-hot GPIO_PIN_x bitmask (GPIO_PIN_0=0x0001 ..
 * GPIO_PIN_15=0x8000) to its 0-15 line/pin index. Shared here so
 * hal_gpio_exti_register() callers and this module's own EXTI dispatch
 * use the same bit-index logic instead of two independently-maintained
 * copies. gpio_pin must be nonzero (single bit set) — undefined
 * otherwise, same as the underlying __builtin_ctz(). */
uint8_t hal_gpio_pin_to_line(uint16_t gpio_pin);

#endif /* HAL_GPIO_H */
