# WP9 — BME280 Environmental Sensor (Temperature, Pressure, Humidity) — Status

**Goal:** read temperature, pressure, and relative humidity from a Bosch Sensortec BME280
(the task description called it "Siemens" — the datasheet on file, and the part itself, is
Bosch Sensortec's BME280) at 1 Hz, sharing I2C1 with the EEPROM. This is a third, independent
temperature source — not the on-board TMP236 or the future external LM35. Branches from
`wp8@e22cb0f` (the tip after WP8's regen reconciliation and code-review fixes).

---

## Requirements (as specified)

- BME280, I2C, shares I2C1 with the EEPROM (24LC256).
- Needs `3V3_EN` (same as every other on-board sensor).
- `SDO` tied to GND, `CSB` tied to VDD (3.3 V).
- Read temperature, pressure, and humidity at ~1 Hz.

## Pin/wiring sourcing

No new pins — I2C1 (`PB6`/`PB7`, already configured for the EEPROM) is reused as-is. I2C
addressing is per-transaction, unlike SPI's per-device CS pins, so a second device on an
already-configured bus needs no CubeMX changes at all — the first time in this project a WP
hasn't needed a regen.

- `SDO = GND` → 7-bit address `1110110b = 0x76` (datasheet §6.2: "Connecting SDO to GND
  results in slave address 1110110 (0x76)").
- `CSB = VDDIO` → I2C interface selected (datasheet §6.1: "If CSB is connected to VDDIO,
  the I²C interface is active"). Matches the wiring as given — confirms the hardware is
  correctly configured for I2C, not accidentally left in SPI mode.

## Register-level design (from the datasheet)

- **Chip ID** (`0xD0`): must read `0x60` for BME280 (`0x56`/`0x57`/`0x58` would be a BMP280 —
  same register map for pressure/temperature but no humidity sensor at all). Checked once at
  init as a sanity/presence check.
- **Soft reset** (`0xE0` ← `0xB6`): explicit reset rather than relying solely on POR, same
  reasoning as WP8's ADS131M04 SYNC/RESET pulse — deterministic starting state regardless of
  what happened before this boot.
- **Calibration trim parameters** (`0x88`–`0xA1`, 26 bytes; `0xE1`–`0xE7`, 7 bytes): factory-
  programmed, read once at init, needed by every later compensation calculation. `dig_H4`/
  `dig_H5` share register `0xE5`'s two nibbles in a well-known BME280 quirk (Table 16) —
  transcribed directly from Bosch's own reference driver formula rather than re-derived, since
  this exact layout is a common source of transcription bugs.
- **Oversampling/mode** (`ctrl_hum` `0xF2`, `ctrl_meas` `0xF4`): ×1 temperature / ×1 pressure /
  ×1 humidity, forced mode, IIR filter off (`config` `0xF5` = `0x00`) — Bosch's own "humidity
  sensing" recommended profile (datasheet Table 8: forced mode, 1 sample/second,
  `osrs_t=×1`/`osrs_h=×1`) extended to also sample pressure at ×1 instead of skipping it, since
  the task needs all three. `ctrl_hum` must be written *before* `ctrl_meas` — changes to
  `ctrl_hum` only take effect on the next `ctrl_meas` write (datasheet §5.4.3), and the
  `ctrl_meas` write is what actually triggers the forced-mode conversion.
- **Conversion timing**: datasheet Appendix B §9.1 gives `t_measure_max = 1.25 + 2.3 +
  (2.3+0.575) + (2.3+0.575) ≈ 9.3 ms` at these oversampling settings. Short enough that
  `drv_bme280_update()` triggers the conversion and then polls the `status` register's
  `measuring` bit with a bounded blocking wait (20 ms cap) rather than a multi-tick async state
  machine — a deliberate tradeoff of its own (see the code review below for why this is *not*
  the same precedent as the EEPROM's write-cycle poll, and how its worst case got tightened).
- **Data readout**: burst read `0xF7`–`0xFE` (8 bytes: `press_msb/lsb/xlsb`,
  `temp_msb/lsb/xlsb`, `hum_msb/lsb`) — a single burst, per the datasheet's own strong
  recommendation, to avoid "a possible mix-up of bytes belonging to different measurements."
  20-bit `adc_P`/`adc_T`, 16-bit `adc_H`.
- **Compensation formulas**: Bosch's official fixed-point code (datasheet §4.2.3) — `int32_t`
  temperature (with the `t_fine` intermediate carried into the pressure/humidity formulas),
  `int64_t` pressure, `int32_t` humidity. Transcribed verbatim rather than re-derived, per the
  datasheet's own strong recommendation ("it is strongly advised to use the API available from
  Bosch Sensortec"); all-integer, so this needed no floats anywhere, unlike WP8's DFT.
- **Output scaling**: temperature is already 0.01 °C/LSB from the compensation formula
  directly (`5123` = 51.23 °C), no further scaling needed. Pressure comes out in Q24.8 format
  (24 integer + 8 fractional bits) — stored as plain Pa (`>>8`, dropping the sub-Pa fraction,
  which is well below the sensor's own ~0.2–3 Pa RMS noise floor anyway). Humidity comes out in
  Q22.10 format — stored as 0.01 %RH/LSB (`(raw * 100) >> 10`), matching TMP236's centi-degree
  convention.

## Non-critical-sensor error handling

Unlike WP7/WP8's DAC/ADC (the instrument's core precision-measurement chain, where an init
failure calls `Error_Handler()`), the BME280 is auxiliary — a missing or faulty part
shouldn't brick the instrument's primary inclination-measurement function. `main.c` calls
`drv_bme280_init()` without checking its return, matching the established pattern for
`drv_tmp236_init()`/`drv_buzzer_init()`/`drv_encoder_init()`. `drv_bme280.c` guards itself
internally (`s_initialized`) so a failed init can't run the compensation formulas against
garbage calibration data; `g_system_state.bme280_ok` simply never becomes `true` if init
failed, which is the CLAUDE.md 7.6 escalation here — observable via system state, just not
boot-halting. A saturating `drv_bme280_get_error_count()` covers later per-cycle failures
(I2C error, status-poll timeout, or a cycle skipped because I2C1 was busy with an EEPROM
transfer).

## Hot-plug tolerance

The BME280 is on a header and can be unplugged, or plugged in for the first time, at any
point during runtime — not just at boot. This needs explicit handling beyond the basic error
path above:

- **Disconnected after a successful boot** — `drv_bme280_update()` returns `DrvStatus`
  reflecting *this cycle's* outcome; `App/app_scheduler.c`'s `task_bme280()` keys
  `g_system_state.bme280_ok` off that return value directly, not off
  `drv_bme280_get_result()`'s (which would keep returning `DRV_OK` — "a reading exists" —
  forever, even for a long-stale value). `bme280_ok` correctly flips to `false` the very
  cycle the module stops responding.
- **Never connected at boot, then plugged in later** — this was the actual gap: the original
  design called `try_init()` (then just `drv_bme280_init()`) exactly once, from `main.c`; if
  that failed, `s_initialized` stayed `false` forever and `drv_bme280_update()`'s very first
  check short-circuited every future call without ever touching the bus again. Fixed:
  `drv_bme280_update()` now retries the full init sequence itself whenever `!s_initialized`,
  and falls through to attempt a real reading in the same call if that succeeds — minimizing
  the delay between plugging the module in and the first reported reading, rather than
  waiting a further scheduler tick.
- **Any comm failure during a normal read cycle** (write NAK'd, read failed, status-poll
  timeout) now also clears `s_initialized`, forcing a full clean re-init (reset, chip-ID
  check, fresh calibration readout) before the next reading is trusted — rather than assuming
  stale calibration data and a half-written register configuration are still good after
  whatever caused the failure. Covers both an outright unplug and a bus glitch severe enough
  that blindly resuming would be a gamble.
- `hal_i2c_is_busy(HAL_I2C_MAIN)` (shared-bus contention with the EEPROM) is deliberately
  *not* treated as a comm failure — that's purely about our own bus scheduling and has
  nothing to do with whether the BME280 itself is present, so it doesn't force a re-init.

Net effect: `g_system_state.bme280_ok` is a live, continuously-accurate "is the module
present and responding right now" signal in all three directions (never connected, connected
then removed, removed then reconnected) — not just a one-shot "did it work at boot" latch.

## Implementation

- `Config/pin_config.h` — a documentation-only note on the existing I2C1 section (no new
  port/pin macros needed).
- `Config/config.h` — `BME280_CTRL_HUM_VALUE`/`BME280_CTRL_MEAS_VALUE`/`BME280_CONFIG_VALUE`
  (the three register values above) and `DEFAULT_TASK_BME280_MS` (1000, fixed literal — no
  `DeviceSettings` room left, same reasoning as `DEFAULT_TASK_UART_MS`).
- `Drivers_App/drv_bme280.c`/`.h` (new) — full register-level driver: `try_init()` (reset,
  `im_update` wait, chip-ID check, calibration readout — factored out so it's re-runnable, not
  just a boot-time step), `drv_bme280_update()` (retries `try_init()` if not currently
  initialised, then trigger + bounded poll + burst read + compensate, one full cycle per
  call), `drv_bme280_get_result()`, `drv_bme280_get_error_count()`. See "Hot-plug tolerance"
  above for why `try_init()` isn't just called once.
- `system_state.h` — `bme280_temp_cdeg`/`bme280_pressure_pa`/`bme280_humidity_centipct`/
  `bme280_ok` fields, clearly distinguished in comments from the pre-REV-B
  `temperature_cdeg` field and the on-board TMP236.
- `App/app_scheduler.c` — new `task_bme280` entry (`DEFAULT_TASK_BME280_MS` period), mirrors
  `task_temperature()`'s TMP236 pattern; no `Services/` wrapper needed since
  `drv_bme280.c` already does all the compensation math itself. Keys `bme280_ok` off
  `drv_bme280_update()`'s per-cycle return value, not `drv_bme280_get_result()`'s.
- `Core/Src/main.c` — `drv_bme280_init()` called once, after `drv_24lc256_init()` (both share
  I2C1) — a fast-path first init, not the only chance the module gets (see above).
- `HAL_App/hal_i2c.c` — `I2C_TIMEOUT_MS` tightened from `100` to `10` (code review finding,
  see below).

**Build-verified clean, first try — no CubeMX regen needed.** Compiles and links with zero
warnings. RAM 34.6 KB (23.5%), FLASH 141.3 KB (26.9%).

## Code review (2026-08-18)

8 parallel angles (line-by-line diff scan, removed-behavior audit, cross-file tracer, reuse,
simplification, efficiency, altitude, CLAUDE.md conventions). Three real correctness bugs
found and fixed, plus one documentation error; the rest were judged acceptable tradeoffs or
pre-existing precedent, left as-is:

- **Fixed, most severe: a stuck/disconnected sensor could stall the whole cooperative
  scheduler for hundreds of ms.** `HAL_App/hal_i2c.c`'s blocking-transaction functions
  (`hal_i2c_write`/`hal_i2c_read`/`hal_i2c_write_read`) had always existed but were unused
  by anything until this WP — the EEPROM driver only ever used the DMA variants. Their shared
  `I2C_TIMEOUT_MS` was `100`, undermining `drv_bme280.c`'s own documented ~9.3–20 ms budget:
  `drv_bme280_update()` makes 5–6 such calls per cycle, so a non-ACKing device could stall up
  to ~500 ms once per second, starving encoder debounce, buzzer timing, and UI/BLE/USB
  polling. Fixed by tightening `I2C_TIMEOUT_MS` to `10` — safe since `drv_bme280.c` is
  confirmed the sole caller of these functions, verified by grep across the whole repo.
- **Fixed: `g_system_state.bme280_ok` never reset to `false` on a later failure.** It was
  only ever set `true` on success; `drv_bme280_get_result()` keeps returning `DRV_OK` (the
  last good cached reading) forever once the sensor stops responding, so nothing downstream
  could tell it had died. Fixed by changing `drv_bme280_update()` to return `DrvStatus`
  reflecting *this* cycle's outcome, and having `task_bme280()` key `bme280_ok` off that
  return value directly instead of `get_result()`'s.
- **Fixed: `drv_bme280_init()`'s `im_update` poll silently proceeded past its timeout**
  instead of returning an error, risking a read of calibration data mid-NVM-copy on a
  marginal board (slow power ramp, cold start). Now returns `DRV_ERR_TIMEOUT` explicitly if
  the bit never clears.
- **Fixed (lower confidence, self-correcting but cheap to close): the status-poll loop had
  no guard that the "measuring" bit had actually been set** before the first check, risking a
  read of the *previous* cycle's stale data if polled too early. Added a small fixed 2 ms
  delay before the first status check — free in the common case, since the ~9.3 ms real
  conversion time is unavoidable regardless.
- **Fixed: a comment misattributed the blocking status-poll design as "the same tradeoff" as
  `drv_24lc256.c`'s EEPROM write-cycle poll** — that precedent is actually a non-blocking,
  multi-tick state machine, not a blocking wait. Corrected to describe this as its own
  tradeoff, justified by the now-tightened worst-case bound, not by a (mischaracterized)
  precedent.
- **Not applied**: a full async/DMA-driven state machine (matching `drv_24lc256.c`'s pattern)
  would be the architecturally deeper fix, reducing scheduler blocking further — judged
  disproportionate scope for this first pass given the timeout fix already bounds the fault
  case tightly. A 5th independent copy of the saturating-counter idiom (`note_error()`) was
  flagged but left as-is, consistent with 4 pre-existing unconsolidated copies elsewhere.
  `drv_bme280_get_error_count()` staying driver-internal rather than promoted to
  `g_system_state` (unlike `usb_tx_dropped_count` et al.) was judged fine given the now-fixed
  `bme280_ok` already provides real-time health observability. `main.c` not checking
  `drv_bme280_init()`'s return / no `DBG_PRINT` call mirrors a pre-existing gap already present
  for `drv_tmp236_init()`/`drv_buzzer_init()`/`drv_encoder_init()`, not a WP9-specific
  regression.

**Rebuilt clean after all fixes: compiles and links with zero warnings.**
RAM 34.6 KB (23.5%), FLASH 141.3 KB (26.9%).

## Not yet done

- **Real-hardware verification** — same caveat as every prior WP: nothing here has touched
  real silicon. The compensation math and register sequencing are transcribed carefully from
  the datasheet, but the actual readings' accuracy is unverified until flashed.
- **`im_update`/`STATUS_POLL_TIMEOUT_MS` reuse**: the init-time `im_update` wait and the
  per-cycle `measuring` wait share the same 20 ms timeout constant even though they bound
  different things (NVM copy vs. conversion time) — both comfortably covered by the same
  margin today, but worth splitting if either ever needs independent tuning.
- **Async/DMA-driven state machine** — deferred, see code review above; would reduce
  scheduler blocking further at the cost of significantly more implementation complexity.
