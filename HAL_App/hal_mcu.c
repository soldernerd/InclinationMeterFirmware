#include "hal_mcu.h"
#include "stm32g0xx_hal.h"

uint32_t hal_mcu_uid_low(void)
{
    /* Folds all three 32-bit words of the STM32G0B1's 96-bit factory UID
     * (UID_BASE, stm32g0b1xx.h) into one 32-bit value by addition — the
     * same technique USB_Device/App/usbd_desc.c's CubeMX-generated
     * Get_SerialNum() already uses for the USB iSerialNumber descriptor
     * (its deviceserial0 += deviceserial2).
     *
     * Returning UID_BASE+0x8 alone (the third word) is NOT safe: ST's
     * silicon documents that word as (part of) the wafer LOT NUMBER,
     * shared by every die manufactured in the same lot/wafer batch — not
     * a per-chip-unique value. Confirmed 2026-09-25: two different REV B
     * bench boards, from the same lot, both reported the identical API v2
     * IDENTITY serial_str with the old single-word read. UID_BASE+0x0 (the
     * X/Y wafer coordinate) is the word that actually varies per die;
     * folding all three in restores genuine per-chip uniqueness without
     * having to pick just one word and hope it's the right one.
     *
     * Plain memory-mapped reads — no peripheral, no clock to enable. */
    uint32_t word0 = *(volatile uint32_t *)(UID_BASE + 0x0U);
    uint32_t word1 = *(volatile uint32_t *)(UID_BASE + 0x4U);
    uint32_t word2 = *(volatile uint32_t *)(UID_BASE + 0x8U);
    return word0 + word1 + word2;
}
