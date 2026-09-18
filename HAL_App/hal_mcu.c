#include "hal_mcu.h"
#include "stm32g0xx_hal.h"

uint32_t hal_mcu_uid_low(void)
{
    /* UID_BASE+0x8 is the third of the three 32-bit words making up the
     * 96-bit factory UID (stm32g0b1xx.h). A plain memory-mapped read — no
     * peripheral, no clock to enable. */
    return *(volatile uint32_t *)(UID_BASE + 0x8U);
}
