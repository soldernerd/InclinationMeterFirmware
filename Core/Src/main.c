/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_drd_fs.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "hal_gpio.h"
#include "hal_systick.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_i2c.h"
#include "hal_adc.h"
#include "hal_power.h"
#include "system_state.h"
#include "drv_tmp236.h"
#include "drv_24lc256.h"
#include "svc_storage.h"
#include "svc_battery.h"
#include "app_scheduler.h"
#include "app_display.h"
#include "app_leds.h"
#include "stm32g0xx_ll_gpio.h"
#include "stm32g0xx_ll_bus.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* !3V3_EN! (PC6) must be LOW before HAL_Init() touches peripherals — REV B
   * inverted this signal's polarity vs. the old board's active-high LDO_EN
   * (PC0): !3V3_EN! is active LOW (Low = on, High = off). On reset the MCU
   * runs from VBAT directly (LP5907 is gated by this pin) — assert it ASAP
   * so the 3.3 V rail comes up before SysTick needs it. We use LL inline
   * functions here because nothing else has been initialised yet. */
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOC);
  LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_6, LL_GPIO_MODE_OUTPUT);
  LL_GPIO_SetPinSpeed(GPIOC, LL_GPIO_PIN_6, LL_GPIO_SPEED_FREQ_LOW);
  LL_GPIO_SetPinOutputType(GPIOC, LL_GPIO_PIN_6, LL_GPIO_OUTPUT_PUSHPULL);
  LL_GPIO_SetPinPull(GPIOC, LL_GPIO_PIN_6, LL_GPIO_PULL_NO);
  LL_GPIO_ResetOutputPin(GPIOC, LL_GPIO_PIN_6);
  /* Give the 3V3 rail a real head start before HAL_Init()/clock/peripheral
   * init add load. The LP5907 feeds sizeable bulk capacitance; on a fast
   * wake from Standby (drained caps, weak battery-node supply) the inrush
   * to charge it can otherwise sag VDD into brown-out. ~25 ms at 16 MHz
   * HSI, a few cycles per iteration. MX_GPIO_Init() now keeps PC6 LOW
   * (PC6.PinState=RESET in the .ioc), so the rail stays up continuously
   * from here — no toggle-off-and-on during CubeMX peripheral init. */
  for (volatile uint32_t i = 0; i < 400000U; ++i) { __asm__("nop"); }
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM3_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_SPI3_Init();
  MX_TIM6_Init();
  MX_USART3_UART_Init();
  MX_USART6_UART_Init();
  MX_USB_DRD_FS_PCD_Init();
  MX_I2C3_Init();
  /* USER CODE BEGIN 2 */
  hal_gpio_init();
  hal_systick_init();
  /* Record whether this boot resumed from Standby mode (all RAM/state was
   * lost either way — this is a fresh boot regardless) and clear the PWR
   * wake flags. No branching needed here: svc_battery_update() re-derives
   * battery/USB state fresh every tick, so a critical-battery-but-VBUS-
   * present wake naturally starts charging instead of re-entering low
   * power without any special-cased logic — see svc_battery.c. */
  g_system_state.woke_from_standby = hal_power_woke_from_standby();
  hal_spi_init(HAL_SPI_DISPLAY);
  hal_i2c_init(HAL_I2C_MAIN);
  /* ADC calibration failure is NOT boot-halting (CLAUDE.md 7.6 escalation):
   * a wake-from-Standby with the 3V3/VREF rail still settling used to brick
   * the device here in Error_Handler(). hal_adc_init() already retries; if
   * it still fails, flag it and carry on — svc_battery/drv_tmp236 gate on
   * ADC .valid and simply report nothing until a later scan succeeds. */
  g_system_state.adc_ok = hal_adc_init();
  hal_tim_init();

  /* Storage must come before scheduler init — it populates
   * g_device_settings (and g_calibration) which the scheduler reads
   * for its task periods. */
  svc_storage_init();
  svc_battery_init();
  drv_tmp236_init();
  drv_24lc256_init();          /* idempotent — svc_storage_init already calls this */

  app_scheduler_init();
  app_leds_init();
  app_display_init();

  hal_adc_start();             /* kick off the first ADC scan */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  app_scheduler_run();  /* never returns */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 16;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
