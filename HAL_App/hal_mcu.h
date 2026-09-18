#ifndef HAL_MCU_H
#define HAL_MCU_H

#include <stdint.h>

/* STM32G0B1's factory-programmed 96-bit unique device ID (UID_BASE,
 * Drivers/CMSIS/Device/ST/STM32G0xx/Include/stm32g0b1xx.h) — fixed per die,
 * set at manufacture, independent of firmware. USB_Device/App/usbd_desc.c's
 * CubeMX-generated Get_SerialNum() already reads all 96 bits for the USB
 * iSerialNumber string descriptor; this exposes just the last 32 bits for
 * anything above HAL_App that wants a short self-identifying value (the
 * API v2 IDENTITY response's serial_str — see svc_api.c) without reaching
 * into CMSIS registers directly (CLAUDE.md §8.1: upper layers go through
 * HAL_App). */
uint32_t hal_mcu_uid_low(void);

#endif /* HAL_MCU_H */
