# Watchdog, fault capture and service mode

Firmware 0.11.1. **Written and host-tested, NOT yet exercised on hardware** -- the bench plan at the end is the
acceptance test for all three.

## Why

* A HardFault used to spin forever. The display's VCOM toggle runs from a timer interrupt that cannot preempt the fault
  handler, so the panel would sit un-refreshed (the Sharp LCD wants VCOM at least once a second) and the instrument stayed
  dead until a power cycle. A main loop that livelocked (a real earlier bug in the displacement drain) was equally
  invisible.
* The API had no protection for the commands that can ruin a calibration or leave the instrument in the ROM bootloader.

## Supervised window watchdog (`HAL_App/hal_wdt.c`, `Math/math_supervisor.c`)

* Hardware: the **WWDG**, not the IWDG. A started IWDG cannot be stopped and, depending on an option byte, keeps counting in
  Standby -- it would wake the instrument by itself and drain the battery. The WWDG runs from the APB clock, which stops in
  Standby, and is inactive after every wake-up.
* The WWDG times out after only 524 ms (4096 x 128 x 64 / 64 MHz), shorter than the longest legitimate pause of the
  cooperative loop. So it is refreshed from the **1 ms SysTick interrupt**, and only while the main loop proves it is alive
  (the scheduler increments a progress counter on every pass).
* Main loop stuck for `WDT_STALL_LIMIT_MS` (3 s, `Config/config.h`) -> the refreshes stop -> reset within 524 ms.
  Interrupts stuck (a storm at a higher priority, a hang with interrupts off) -> no refreshes at all -> reset within 524 ms.
* Started last in `main()`, so the slow initialisation runs unsupervised. A halted debugger freezes it. `WATCHDOG_ENABLED 0`
  builds without it.
* A watchdog reset is visible after the reboot: System HEALTH `reset_cause` (bit 4 = WWDG) and a boot log line.

## Fault capture (`HAL_App/hal_fault.c`, `Math/math_fault.c`)

* `HardFault_Handler` is a naked three-instruction stub (`Core/Src/stm32g0xx_it.c`, USER CODE) that hands the stacked
  exception frame to `hal_fault_hardfault_c()`. That stores kind, PC, LR, xPSR and SP in the TAMP backup registers (they survive
  a reset, not a loss of power; a check byte keeps power-on garbage from being read as a fault) and calls `NVIC_SystemReset()`.
* `Error_Handler()` (an initialisation step failed) records the caller's address the same way and still stops, so the
  failure is not turned into a boot loop. The watchdog is not running yet at that point.
* The next boot reads the record, logs it at ERROR level and clears it. System HEALTH carries `last_fault_kind`, `_pc`, `_lr`;
  look the address up with `arm-none-eabi-addr2line -e <elf> <pc>` (use the elf of the firmware that was running).
* A fault that recurs at every boot is a boot loop, visible as a rising uptime of a few seconds and the fault in HEALTH; the
  way out is to reflash.

## Service mode (`Services/svc_service.c`)

Protocol side: `docs/api-v3-spec.md` section 5a. Entered only on the instrument (SETTINGS > *Service mode*, right knob
twice), left by the same row, API `SERVICE_END`, 10 minutes without API requests, or sleep. Unlocks the `service=True`
resources of `tools/api_spec.py`: every calibration write and Commands ZERO_CAL, FACTORY_DEFAULTS, REBOOT_DFU, POWER_TEST,
RAIL, PIN_TEST, FAULT_TEST. The SETTINGS row shows `[ON m:ss]` with the time left until the idle timeout.

## Bench plan (to do when a board is free)

1. **Service mode**: with it off, a calibration SET and each gated command answer `SERVICE_MODE_REQUIRED` (try
   `PythonTestCode/set_vbat_scale.py`); reading still works. Switch it on in the menu, the same calls succeed; System STATE
   shows `service_mode = 1`. Leave it with SERVICE_END and with the menu; check the 10-minute idle end with a stopwatch
   (shorten `SERVICE_MODE_IDLE_TIMEOUT_MS` for a quick pass); check that it is off after Standby.
2. **HardFault**: service mode on, `FAULT_TEST` kind 1. Expect the instrument to reset within a moment; HEALTH shows
   `last_fault_kind = 1` with a PC inside `hal_fault`'s caller (`after_fault_test`), the log shows the boot line. The display
   must come back normally.
3. **Main-loop hang**: kind 2. Expect a reset after about 3 s; `reset_cause` bit 4 (WWDG) and the boot log line.
4. **Hang with interrupts off**: kind 3. Expect a reset within about half a second.
5. **Standby**: the watchdog must NOT reset a sleeping instrument. Power off from the menu, leave it for several minutes
   (longer than 3 s and than any watchdog period), wake it with the encoder: it must wake only on the button, show uptime
   from the wake, and `reset_cause` must not show a WWDG reset. Repeat with the auto power-off timeout and with the
   critical-battery path if convenient.
6. **No false trips under load**: a 24 h run with the display, USB / BLE / UART traffic, a bulk capture and the phasor stream
   active must show no WWDG reset (`reset_cause`, uptime keeps growing) and unchanged `input_drop` figures.
7. **Debugger**: with a debugger halted at a breakpoint for a minute the instrument must not reset.
