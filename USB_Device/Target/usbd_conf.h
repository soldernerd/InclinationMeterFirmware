/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_conf.h
  * @version        : v3.0_Cube
  * @brief          : Header for usbd_conf.c file.
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
#ifndef __USBD_CONF__H__
#define __USBD_CONF__H__

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32g0xx.h"
#include "stm32g0xx_hal.h"

/* USER CODE BEGIN INCLUDE */
#include "config.h"

/* Overrides the Class/CustomHID/Inc/usbd_customhid.h #ifndef defaults
 * (2 bytes, sized for the ST template's tiny LED-report use case) to our
 * full 64-byte vendor report. Effective here (rather than down in the
 * "Exported Defines" section below, where CubeMX's own regen output
 * lives) because usbd_customhid.h pulls in this whole file — top to
 * bottom, this INCLUDE block included — via usbd_ioreq.h -> usbd_def.h
 * -> usbd_conf.h before its own #ifndef guards run, so definition order
 * within this file doesn't matter as long as it's above the point
 * usbd_customhid.h itself gets included from some other translation
 * unit. What DOES matter: this needs to be inside a USER CODE marker.
 * Placed in the "Exported Defines" section (outside any marker) twice
 * before, it was silently deleted by a CubeMX regen both times — this
 * USER CODE BEGIN/END INCLUDE block is the one place in this file
 * confirmed to survive a regen (docs/wp2-5_rebase_status.md's "wp4"
 * section has the history). The CubeMX GUI has no field for either
 * macro, so this can't be fixed by ticking a checkbox the way the
 * EXTI NVIC issue was — re-verify these two lines are still here after
 * any future regen. */
#define CUSTOM_HID_EPIN_SIZE     USB_HID_REPORT_SIZE
#define CUSTOM_HID_EPOUT_SIZE    USB_HID_REPORT_SIZE
/* USER CODE END INCLUDE */

/** @addtogroup USBD_OTG_DRIVER
  * @brief Driver for Usb device.
  * @{
  */

/** @defgroup USBD_CONF USBD_CONF
  * @brief Configuration file for Usb otg low level driver.
  * @{
  */

/** @defgroup USBD_CONF_Exported_Variables USBD_CONF_Exported_Variables
  * @brief Public variables.
  * @{
  */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* USER CODE END PV */
/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Defines USBD_CONF_Exported_Defines
  * @brief Defines for configuration of the Usb device.
  * @{
  */

/*---------- -----------*/
#define USBD_MAX_NUM_INTERFACES     1U
/*---------- -----------*/
#define USBD_MAX_NUM_CONFIGURATION     1U
/*---------- -----------*/
#define USBD_MAX_STR_DESC_SIZ     512U
/*---------- -----------*/
#define USBD_DEBUG_LEVEL     0U
/*---------- -----------*/
#define USBD_LPM_ENABLED     1U
/*---------- -----------*/
#define USBD_SELF_POWERED     1U
/*---------- -----------*/
#define USBD_CUSTOMHID_OUTREPORT_BUF_SIZE     64U
/*---------- -----------*/
#define USBD_CUSTOM_HID_REPORT_DESC_SIZE     29U
/*---------- -----------*/
#define CUSTOM_HID_FS_BINTERVAL     0x5U

/****************************************/
/* #define for FS and HS identification */
#define DEVICE_FS 		0

/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Macros USBD_CONF_Exported_Macros
  * @brief Aliases.
  * @{
  */

/* Memory management macros make sure to use static memory allocation */
/** Alias for memory allocation. */
#define USBD_malloc         (void *)USBD_static_malloc

/** Alias for memory release. */
#define USBD_free           USBD_static_free

/** Alias for memory set. */
#define USBD_memset         memset

/** Alias for memory copy. */
#define USBD_memcpy         memcpy

/** Alias for delay. */
#define USBD_Delay          HAL_Delay

/* DEBUG macros */

#if (USBD_DEBUG_LEVEL > 0)
#define USBD_UsrLog(...)    printf(__VA_ARGS__);\
                            printf("\n");
#else
#define USBD_UsrLog(...)
#endif

#if (USBD_DEBUG_LEVEL > 1)

#define USBD_ErrLog(...)    printf("ERROR: ") ;\
                            printf(__VA_ARGS__);\
                            printf("\n");
#else
#define USBD_ErrLog(...)
#endif

#if (USBD_DEBUG_LEVEL > 2)
#define USBD_DbgLog(...)    printf("DEBUG : ") ;\
                            printf(__VA_ARGS__);\
                            printf("\n");
#else
#define USBD_DbgLog(...)
#endif

/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Types USBD_CONF_Exported_Types
  * @brief Types.
  * @{
  */

/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_FunctionsPrototype USBD_CONF_Exported_FunctionsPrototype
  * @brief Declaration of public functions for Usb device.
  * @{
  */

/* Exported functions -------------------------------------------------------*/
void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);

/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF__H__ */

