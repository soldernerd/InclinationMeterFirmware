/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define DAC_SYNC_Pin GPIO_PIN_13
#define DAC_SYNC_GPIO_Port GPIOC
#define CHARGE_SENSE_Pin GPIO_PIN_2
#define CHARGE_SENSE_GPIO_Port GPIOC
#define ENC_1A_Pin GPIO_PIN_4
#define ENC_1A_GPIO_Port GPIOC
#define ENC_1A_EXTI_IRQn EXTI4_15_IRQn
#define ENC_1B_Pin GPIO_PIN_0
#define ENC_1B_GPIO_Port GPIOB
#define ENC_1B_EXTI_IRQn EXTI0_1_IRQn
#define ENC_2A_Pin GPIO_PIN_1
#define ENC_2A_GPIO_Port GPIOB
#define ENC_2A_EXTI_IRQn EXTI0_1_IRQn
#define ENC_2B_Pin GPIO_PIN_2
#define ENC_2B_GPIO_Port GPIOB
#define ENC_2B_EXTI_IRQn EXTI2_3_IRQn
#define _3V3_ENABLE__Pin GPIO_PIN_6
#define _3V3_ENABLE__GPIO_Port GPIOC
#define STANDBY_SENSE_Pin GPIO_PIN_5
#define STANDBY_SENSE_GPIO_Port GPIOD
#define BLE_RESET_Pin GPIO_PIN_5
#define BLE_RESET_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
