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
#include "drv_encoder.h"
#include "drv_buzzer.h"
#include "svc_storage.h"
#include "svc_battery.h"
#include "app_scheduler.h"
#include "app_display.h"
#include "app_leds.h"
#include "app_ui.h"
#include "svc_input.h"
#include "pin_config.h"
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
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* Deliberate boot order (agreed 2026-09-03): each step is a checkpoint
   * you can observe on LED_STS without a debugger attached — if it stops
   * toggling, that names the stretch that's hanging.
   *   1. GPIO init                    5. 3V3 rail + settle
   *   2. LED_PWR on                   6. 5V rail + settle
   *   3. HSE/PLL + timers             7. internal ADC (no digital bus)
   *   4. LED_STS starts as a heartbeat 8. digital-bus peripherals, gated
   *      (checkpoint toggles here,       on their rail: I2C1/EEPROM (3V3),
   *      real 2 Hz blink once the        SPI2/display (5V)
   *      scheduler starts)
   * !3V3_EN! is touched exactly once now (step 5) — no more pre-HAL_Init
   * head-start spin: that existed only for the brown-out theory, which is
   * ruled out (BOR is off in the option bytes; raising supply voltage to
   * normal made no difference either). */
  /* USER CODE END Init */

  /* 1. GPIO */
  MX_GPIO_Init();

  /* 2. LED_PWR on — the only thing this depends on is PB13 being
   * configured as an output, done by MX_GPIO_Init() just above. Proof of
   * life before anything that could actually hang. */
  app_leds_init();
  hal_gpio_init();              /* remaining pin defaults — no rails, no LEDs (see hal_gpio.c) */
  hal_systick_init();
  g_system_state.woke_from_standby = hal_power_woke_from_standby();

  /* 3. External crystal + PLL, then timers. HAL_RCC_ClockConfig() re-syncs
   * SysTick to the new SystemCoreClock internally, so HAL_Delay() below
   * (steps 5-6) is accurate regardless of running on HSI up to this point. */
  SystemClock_Config();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: HSE/PLL locked */

  MX_DMA_Init();                /* must precede any peripheral that DMAs (SPI2, I2C1 below) */
  MX_TIM3_Init();                /* buzzer PWM base — not used until WP3, harmless to configure now */
  MX_TIM6_Init();                /* VCOM base timer — drv_sharp_lcd_init() starts it later */
  hal_tim_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: timers configured */

  /* 4. LED_STS heartbeat starts here (checkpoint toggles through the rest
   * of this function); app_scheduler_run() hands it off to the real 2 Hz
   * task_leds() once the scheduler is alive. */

  /* 5. 3.3V rail. Needed in WP2 for the battery-sense divider (its
   * low-side gate needs this rail live so the divider doesn't otherwise
   * bleed the battery while "off") and the EEPROM. Scoped: the rail is
   * fully up within a couple of ms of the enable; 5 ms is ample. */
  hal_gpio_set(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN, false);   /* !3V3_EN! active-LOW: LOW = rail ON */
  HAL_Delay(5);
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: 3.3V rail settled */

  /* 6. 5V rail. Needed for the display and both temp sensors. Bringing it
   * (and the -5V inverter off it) up transiently sags the shared battery
   * node, dipping 3.3V ~0.7 V for ~23 ms before it recovers (scoped).
   * 30 ms here so everything — including that 3.3V recovery — is fully
   * settled before the ADC/VREF work in step 7. */
  hal_gpio_set(PWR_5V_EN_PORT, PWR_5V_EN_PIN, true);
  HAL_Delay(40);   /* 30 ms was just enough on the scope; 40 for margin */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: 5V rail settled */

  /* 7. Internal ADC — no digital bus, no external chip to wait on beyond
   * VREF+/VDDA (always-on rail) already being stable. */
  MX_ADC1_Init();
  /* ADC calibration failure is NOT boot-halting (CLAUDE.md 7.6 escalation):
   * a wake-from-Standby with the 3V3/VREF rail still settling used to brick
   * the device here in Error_Handler(). hal_adc_init() already retries; if
   * it still fails, flag it and carry on — svc_battery/drv_tmp236 gate on
   * ADC .valid and simply report nothing until a later scan succeeds. */
  g_system_state.adc_ok = hal_adc_init();
  drv_tmp236_init();             /* no-op: reads via the same internal ADC, no bus of its own */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: ADC calibration done */

  /* 8a. I2C1 / EEPROM — needs the 3.3V rail (step 5) up. */
  MX_I2C1_Init();
  hal_i2c_init(HAL_I2C_MAIN);
  /* Storage must come before scheduler init — it populates
   * g_device_settings (and g_calibration) which the scheduler reads
   * for its task periods. */
  svc_storage_init();
  drv_24lc256_init();            /* idempotent — svc_storage_init already calls this */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: EEPROM load/seed done */

  /* 8b. SPI2 / display — needs the 5V rail (step 6) up. */
  MX_SPI2_Init();
  hal_spi_init(HAL_SPI_DISPLAY);
  app_display_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: display init kicked off */

  /* 8c. Local UI layer (WP3): rotary encoders (EXTI on GPIO from step 1),
   * push-switch polling, buzzer PWM (TIM3 from step 4 + 5V rail from
   * step 6), and the UI state machine. All before the scheduler starts
   * pumping task_input / task_buzzer / task_ui. */
  drv_encoder_init(ENCODER_1);
  drv_encoder_init(ENCODER_2);
  drv_buzzer_init();
  svc_input_init();
  app_ui_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: UI layer up */

  /* Remaining CubeMX peripherals: not yet used by any current WP (external
   * ADC/DAC front end on SPI1/SPI3, RN4871 BLE UART, USB, a third I2C bus).
   * Deferred to last on purpose — WP2/WP1's own init is what matters for
   * "did we get far enough to work", not this boilerplate. */
  MX_SPI1_Init();
  MX_SPI3_Init();
  MX_USART3_UART_Init();
  MX_USART6_UART_Init();
  MX_USB_DRD_FS_PCD_Init();
  MX_I2C3_Init();

  /* USER CODE BEGIN 2 */
  svc_battery_init();
  app_scheduler_init();
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
