#include "hal_dfu.h"
#include "hal_usb.h"
#include "stm32g0xx_hal.h"

void hal_dfu_enter_bootloader(void)
{
    /* Hold D+ low long enough for the host to register a real disconnect
     * (well past its debounce) before we vanish — otherwise the host
     * keeps the stale application enumeration and never re-enumerates the
     * device as USB DFU after the option-byte reload. Harmless when USB
     * is not the trigger transport. */
    hal_usb_detach();
    HAL_Delay(400);

    /* Flash option-byte programming must not be interrupted. */
    __disable_irq();

    if (HAL_FLASH_Unlock() == HAL_OK && HAL_FLASH_OB_Unlock() == HAL_OK) {
        FLASH_OBProgramInitTypeDef ob = { 0 };
        ob.OptionType = OPTIONBYTE_USER;
        /* nBOOT_SEL = 1 : boot source is the nBOOT0 bit, not the PA14/BOOT0
         *                 pin (which is otherwise a GPIO here).
         * nBOOT0    = 0 : that boot source selects system memory.          */
        ob.USERType   = OB_USER_nBOOT_SEL | OB_USER_nBOOT0;
        ob.USERConfig = OB_BOOT0_FROM_OB  | OB_nBOOT0_RESET;

        if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK) {
            /* Reloads the option bytes and resets the MCU. Boot config is
             * re-sampled -> ROM bootloader. Does not return. */
            HAL_FLASH_OB_Launch();
        }
    }

    /* Reached only if unlock or OB programming failed. Restore lock state
     * and fall back to a plain reset rather than leave things half-done. */
    (void)HAL_FLASH_OB_Lock();
    (void)HAL_FLASH_Lock();
    NVIC_SystemReset();
    for (;;) { }
}
