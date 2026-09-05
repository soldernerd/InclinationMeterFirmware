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
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "hal_gpio.h"
#include "hal_systick.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_i2c.h"
#include "hal_adc.h"
#include "hal_uart.h"
#include "hal_power.h"
#include "hal_rtc.h"
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
#include "svc_usb.h"
#include "svc_ble.h"
#include "svc_uart.h"
#include "svc_power.h"
#include "svc_api.h"
#include "svc_log.h"
#include "svc_measurement.h"
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

/* --- TEMP boot diagnostic (v0.6.3) — remove after locating the wake hang ---
 * Blinks LED_PWR (PB13, active high) n times, raw busy-wait, no dependency
 * on the HAL / SysTick / system clock. Count the LAST full group you see
 * before it freezes; the step listed for count N+1 is the one that hung:
 *   1  reached main()              9  after MX_I2C1_Init
 *   2  after SystemClock_Config    10 after MX_SPI3_Init
 *   3  after MX_GPIO_Init          11 after MX_TIM6_Init
 *   4  after MX_DMA_Init           12 after MX_USART3_UART_Init
 *   5  after MX_TIM3_Init          13 after MX_USART6_UART_Init
 *   6  after MX_SPI1_Init          14 after MX_I2C3_Init
 *   7  after MX_SPI2_Init          15 after MX_USB_Device_Init
 *   8  after MX_ADC1_Init          16 after MX_RTC_Init (normal boot follows) */
static void dbg_cp(int n)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (13U * 2U))) | (1UL << (13U * 2U));
    for (int k = 0; k < n; k++) {
        GPIOB->BSRR = (1UL << 13U);
        for (volatile uint32_t d = 0; d < 350000UL; d++) { }
        GPIOB->BSRR = (1UL << (13U + 16U));
        for (volatile uint32_t d = 0; d < 350000UL; d++) { }
    }
    for (volatile uint32_t d = 0; d < 3000000UL; d++) { }   /* long gap between groups */
}

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
  dbg_cp(1);   /* TEMP v0.6.2: reached main(), before SystemClock_Config() */
  /* Deliberate boot order (agreed 2026-09-03; restructured 2026-09-05 —
   * see the long comment in USER CODE 2 below for why and where the rest
   * of this sequence now lives). Checked/cleared as early as possible,
   * before anything else touches PWR. */
  g_system_state.woke_from_standby = hal_power_woke_from_standby();
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  dbg_cp(2);   /* TEMP v0.6.2: SystemClock_Config() returned OK */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();          dbg_cp(3);   /* TEMP v0.6.3 */
  MX_DMA_Init();           dbg_cp(4);
  MX_TIM3_Init();          dbg_cp(5);
  MX_SPI1_Init();          dbg_cp(6);
  MX_SPI2_Init();          dbg_cp(7);
  MX_ADC1_Init();          dbg_cp(8);
  MX_I2C1_Init();          dbg_cp(9);
  MX_SPI3_Init();          dbg_cp(10);
  MX_TIM6_Init();          dbg_cp(11);
  MX_USART3_UART_Init();   dbg_cp(12);
  MX_USART6_UART_Init();   dbg_cp(13);
  MX_I2C3_Init();          dbg_cp(14);
  MX_USB_Device_Init();    dbg_cp(15);
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */
  dbg_cp(16);   /* TEMP v0.6.3: all MX_*_Init() done, entering USER CODE 2 */
  /* Deliberate boot order (agreed 2026-09-03). Originally interleaved
   * directly between the individual MX_*_Init() calls above, each step a
   * checkpoint observable on LED_STS without a debugger attached — but
   * that section is CubeMX-owned and gets fully regenerated on every
   * regen (lost whole, silently, on 2026-09-05's RTC/USB clock change —
   * caught only by diffing before committing). Restructured to live
   * entirely here instead, in the one block CubeMX has always left alone:
   * every MX_*_Init() above only configures registers/pins, none of them
   * touch a live bus or need a rail up yet, so it's safe to let CubeMX
   * run them all first, in whatever order it wants, and do the actual
   * rail sequencing and bus-touching init below, in the same relative
   * order and with the same scoped delays as before.
   *   1. LEDs/GPIO proof of life        4. internal ADC (no digital bus)
   *   2. 3V3 rail + settle              5. digital-bus peripherals, gated
   *   3. 5V rail + settle                  on their rail: I2C1/EEPROM
   *                                         (3V3), SPI2/display (5V)
   * !3V3_EN! is touched exactly once (step 2) — no pre-HAL_Init head-start
   * spin: that existed only for the brown-out theory, which is ruled out
   * (BOR is off in the option bytes; raising supply voltage to normal made
   * no difference either). */
  app_leds_init();
  hal_gpio_init();              /* remaining pin defaults — no rails, no LEDs (see hal_gpio.c) */
  hal_systick_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: GPIO/LEDs up */

  hal_tim_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: timers configured */

  /* 3.3V rail. Needed in WP2 for the battery-sense divider (its low-side
   * gate needs this rail live so the divider doesn't otherwise bleed the
   * battery while "off") and the EEPROM. Scoped: the rail is fully up
   * within a couple of ms of the enable; 5 ms is ample. */
  hal_gpio_set(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN, false);   /* !3V3_EN! active-LOW: LOW = rail ON */
  HAL_Delay(5);
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: 3.3V rail settled */

  /* 5V rail. Needed for the display and both temp sensors. Bringing it
   * (and the -5V inverter off it) up transiently sags the shared battery
   * node, dipping 3.3V ~0.7 V for ~23 ms before it recovers (scoped).
   * 30 ms here so everything — including that 3.3V recovery — is fully
   * settled before the ADC/VREF work below. */
  hal_gpio_set(PWR_5V_EN_PORT, PWR_5V_EN_PIN, true);
  HAL_Delay(40);   /* 30 ms was just enough on the scope; 40 for margin */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: 5V rail settled */

  /* Internal ADC — no digital bus, no external chip to wait on beyond
   * VREF+/VDDA (always-on rail) already being stable.
   * ADC calibration failure is NOT boot-halting (CLAUDE.md 7.6 escalation):
   * a wake-from-Standby with the 3V3/VREF rail still settling used to brick
   * the device here in Error_Handler(). hal_adc_init() already retries; if
   * it still fails, flag it and carry on — svc_battery/drv_tmp236 gate on
   * ADC .valid and simply report nothing until a later scan succeeds. */
  g_system_state.adc_ok = hal_adc_init();
  drv_tmp236_init();             /* no-op: reads via the same internal ADC, no bus of its own */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: ADC calibration done */

  /* I2C1 / EEPROM — needs the 3.3V rail above up. */
  hal_i2c_init(HAL_I2C_MAIN);
  /* Storage must come before scheduler init — it populates
   * g_device_settings (and g_calibration) which the scheduler reads
   * for its task periods. */
  svc_storage_init();
  drv_24lc256_init();            /* idempotent — svc_storage_init already calls this */
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: EEPROM load/seed done */

  /* SPI2 / display — needs the 5V rail above up. */
  hal_spi_init(HAL_SPI_DISPLAY);
  app_display_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: display init kicked off */

  /* Local UI layer (WP3): rotary encoders (EXTI on GPIO), push-switch
   * polling, buzzer PWM (TIM3 + 5V rail above), and the UI state machine.
   * All before the scheduler starts pumping task_input/task_buzzer/task_ui. */
  drv_encoder_init(ENCODER_1);
  drv_encoder_init(ENCODER_2);
  drv_buzzer_init();
  svc_input_init();
  app_ui_init();
  HAL_GPIO_TogglePin(LED_STS_PORT, LED_STS_PIN);   /* checkpoint: UI layer up */

  /* WP4 comms stack. svc_api_init() before the three transport inits:
   * each registers itself via svc_api_register_transport(), which needs
   * the transport table already zeroed. svc_ble_init() also pulses the
   * RN4871's ~RESET and kicks off its (non-blocking) config state
   * machine, pumped thereafter from task_ble. */
  svc_log_init();             /* log ring up before anything logs into it */
  svc_api_init();
  svc_measurement_init();
  svc_usb_init();
  hal_uart_init(HAL_UART_BLE);    /* USART6 RX DMA up before the RN4871 talks */
  svc_ble_init();
  hal_uart_init(HAL_UART_DEBUG);  /* USART3 — the wired debug/VCP API transport */
  svc_uart_init();
  svc_battery_init();
  svc_log(API2_LOG_INFO, "boot: comms stack up");

  /* WP6 power management: RTC (calendar keeps running through Standby) and
   * the auto power-off idle timer. After svc_input_init()/svc_storage_init()
   * so svc_power_init() sees valid encoder state and the loaded
   * auto_poweroff_s. */
  hal_rtc_init();
  svc_power_init();

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

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  /* HAND-EDIT (WP6, 2026-09-05): LSEDRV may only be written while the LSE
   * is stopped (RM0444 "LSEDRV can be written only when LSEON = 0 and
   * LSERDY = 0"). On a wake from Standby the backup domain kept the LSE
   * crystal running, so this write glitches the oscillator; HAL_RCC_OscConfig()
   * below then spins the full RCC_LSE_TIMEOUT_VALUE (5 s) waiting for LSERDY,
   * times out, and main() drops into Error_Handler() — an infinite loop
   * before the first LED. Only (re)configure the drive on a genuine cold
   * start (LSE not yet running); on a warm wake OscConfig sees LSERDY
   * already set and skips its wait. A CubeMX regen will drop this guard —
   * see memory/cubemx-regen-hazards. */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == 0U)
  {
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 24;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV3;
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
