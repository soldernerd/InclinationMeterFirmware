#include "hal_pintest.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim6;

static bool s_armed = false;

static void arm(void)
{
    /* Release the peripherals that own these six pins. */
    HAL_TIM_Base_Stop_IT(&htim6);              /* DISP_VCOM toggle */
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_4);   /* BUZZER PWM (PC9) */
    (void)HAL_SPI_DeInit(&hspi2);              /* frees PD1/PD4 from AF */

    GPIO_InitTypeDef g = {0};
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;

    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4;  /* PD0..PD4 */
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_9;                                                     /* PC9 */
    HAL_GPIO_Init(GPIOC, &g);

    s_armed = true;
}

void hal_pintest_apply(uint8_t pattern, bool allow_disp_on)
{
    if (!s_armed) {
        arm();
    }

    bool disp_on = allow_disp_on && (pattern & 0x08U);

    HAL_GPIO_WritePin(DISP_SCK_PORT,  DISP_SCK_PIN,  (pattern & 0x01U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISP_MOSI_PORT, DISP_MOSI_PIN, (pattern & 0x02U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISP_CS_PORT,   DISP_CS_PIN,   (pattern & 0x04U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISP_ON_PORT,   DISP_ON_PIN,   disp_on            ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISP_VCOM_PORT, DISP_VCOM_PIN, (pattern & 0x10U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BUZZER_PORT,    BUZZER_PIN,    (pattern & 0x20U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
