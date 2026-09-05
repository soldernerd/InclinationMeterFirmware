/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_device.c
  * @version        : v3.0_Cube
  * @brief          : This file implements the USB Device
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

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_customhid.h"
#include "usbd_custom_hid_if.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

extern void Error_Handler(void);
/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceFS;
extern USBD_DescriptorsTypeDef CUSTOM_HID_Desc;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_Device_Init(void)
{
  /* USER CODE BEGIN USB_Device_Init_PreTreatment */

  /* USER CODE END USB_Device_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceFS, &CUSTOM_HID_Desc, DEVICE_FS) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_CUSTOM_HID) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_CUSTOM_HID_RegisterInterface(&hUsbDeviceFS, &USBD_CustomHID_fops_FS) != USBD_OK) {
    Error_Handler();
  }
  /* HAND-EDIT (WP6, 2026-09-05): only USBD_Start() when VBUS is present.
   * With no host, USBD_Start() connects D+/D- into an unpowered bus and the
   * USB IP raises bus-reset/error interrupts at NVIC priority 0 forever,
   * starving SysTick — so every HAL_Delay() further down main()'s boot
   * sequence (rail settle, etc.) never returns and a battery-only power-up
   * hangs before the display. hal_usb_update() (task_usb) starts/stops the
   * stack on VBUS edges from here on, so a cable plugged in after boot
   * still enumerates. PA2 = VBUS_SENSE is left in POR analog mode by
   * MX_GPIO_Init (it doubles as SYS_WKUP4); flip it to a plain digital
   * input just long enough to sample it. A CubeMX regen restores the
   * unconditional USBD_Start() — see memory/cubemx-regen-hazards. */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  MODIFY_REG(GPIOA->MODER, (3UL << (2UL * 2UL)), 0UL);   /* PA2 -> input */
  if (READ_BIT(GPIOA->IDR, (1UL << 2UL)) != 0UL) {       /* VBUS present (active high) */
    if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
      Error_Handler();
    }
  }
  /* USER CODE BEGIN USB_Device_Init_PostTreatment */

  /* USER CODE END USB_Device_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */

