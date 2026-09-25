# WP10 — Differential-Capacitor Phasor Demodulation and Displacement — Status

**Goal:** port the never-merged `wp10` branch's displacement demodulation onto REV B —
per-cycle complex-division math for two differential-capacitor sensor heads (S1, S2)
sharing a common antiphase excitation pair (A, B), fed from the same 4-channel ADS131M04
ADC stream WP8 already brought up. Stops at δ (mm of plate displacement) — converting to
inclination angle needs empirical pendulum/flexure calibration and is later work, same
scope boundary the original branch had.

---

## Where this came from

`wp10` (REV A, branched from `wp9@869d33c`, never merged) already had this exact
physical model and math working — differential-capacitor phasor demod, `x = (S/k − B)/(A −
B)` via `math_phasor.c`'s complex reciprocal, EEPROM-backed per-sensor calibration. It
was shelved when REV A hardware (PCAP04-based) was abandoned. Ported onto master
2026-09-24, on top of REV B's WP7 (AD9833 DDS excitation) + WP8 (ADS131M04 4-channel ADC)
front end, which turned out to already be wired for exactly this: `MATH_PHASOR_SAMPLES_PER_CYCLE`
(8) matches `fDATA/fOUT` (20833.33/2604.167 = exactly 8) by design, not coincidence — WP7/WP8's
clock dividers were chosen with this integration in mind even though it never got connected
until now.

## Channel mapping — confirmed with the user, not guessed

REV A's `wp10` branch mapping (`ch0=B, ch1=A, ch2=S1, ch3=S2`) does **not** carry over — REV
B's front end is wired differently. Confirmed directly:

```
CH0 = Sensor 2 (S2)      CH1 = Exciter B
CH2 = Exciter A          CH3 = Sensor 1 (S1)
```

S1 is connected via an external cable on the bench; S2 is not (yet). This matches
everything observed empirically before the port even started: ch1/ch2 were the
rock-stable, ~180°-antiphase pair (the excitation), ch3 responded to physically moving the
instrument (S1), ch0 stayed flat (S2, unconnected).

## What changed vs. the original `wp10` branch

- **Calibration storage**: the old branch used a standalone `DisplacementSensorCal`/
  `DisplacementSharedCal` struct pair with its own dedicated-page pattern — that storage
  convention no longer exists on REV B (`Services/svc_storage.c` now has exactly one
  pattern, `SettingsSection`/`offsetof` slices of `DeviceSettings`). Ported as **scaled
  integers** inside `DeviceSettings` instead of raw floats — `svc_api.c`'s `SF()` field
  machinery is integer-only, matching every other calibration constant in this codebase.
  Converted to float once per batch in `svc_displacement.c`, where CLAUDE.md permits
  floating point (Math/Services layer, not HAL/driver).
- **API surface**: exposed under **Calibrations (category 0x2)** — the first resources to
  use that category, which had sat reserved-but-unimplemented since the API v2 port.
  `svc_api.c`'s `dispatch()` had a comment literally pointing WP11+ at this seam
  ("copy the `s_settings_fields[]`/`SF()` machinery... `dispatch_calibrations()`") —
  followed verbatim.
- **`svc_signal_analysis.c` (WP8) retired**, not just "not registered a second callback":
  `drv_ads131m04.c` only supports one sample callback at a time, and WP8's generic
  4-channel amplitude/phase diagnostic is fully superseded by this module's math. Its
  Commands surface repointed (`SIGNAL_ANALYSIS` → `DISPLACEMENT`, same wire value 0x01).
  Bulk raw-ADC capture was initially retired outright alongside it (2026-09-24) on the
  theory that it couldn't coexist with the demod owning the one callback slot -- **wrong,
  and corrected 2026-09-25** (see "Bulk raw-ADC capture restored" below): it's an important
  bench diagnostic tool independent of the demod math, and the two only need to be
  mutually exclusive at runtime, not architecturally incompatible.
- **Measurements (0x4)**: `disp1_delta_mm`/`disp1_residual`/`disp2_delta_mm`/
  `disp2_residual`/`disp_ok`, latest-batch snapshot, float32 LE on the wire (the first
  floats this API has sent). The high-rate per-cycle/per-batch stream
  (`svc_displacement_pop()`, `DISP_STREAM` in the old branch) is deferred — nothing drains
  it yet on this first pass.

## Two real bugs found and fixed getting this running

### 1. Callback registered before, not after, `drv_ads131m04_init()`

Ported `svc_displacement_init()` in the same order as the old branch:
`drv_ads131m04_set_on_sample()` then `drv_ads131m04_init()`. On the *current* driver,
`drv_ads131m04_init()` resets its callback pointer to `NULL` as its first action — so the
registration was silently wiped. Symptom: acquisition ran perfectly (`frames_produced`
tracking `frames_drained`, zero faults) but `disp_ok` never went true, because
`on_sample()` was never actually being called. Swapped the call order; the driver's own
header already documented this ("call `_set_on_sample()` AFTER this"), just not followed.

### 2. A real livelock, found with a debugger — not the memory-corruption bug it first looked like

**Symptom:** starting displacement made the board stop answering over UART within about
one sample period — looked exactly like a HardFault (a reset always "fixed" it).

**First (wrong) diagnosis path:** extensive bisection (13+ flash-and-test cycles)
progressively removing pieces of `process_one_cycle()`'s storage step, on the theory this
was a memory-safety bug. Each remove-a-piece step that happened to "fix" it and each that
didn't looked inconsistent enough (some "safe" configurations were only lightly soak-tested)
to smell like a heisenbug, which — correctly, in hindsight — pointed at *timing*, not
memory correctness. That whole investigation was chasing the wrong layer.

**What actually found it:** this bench has no GDB server (no OpenOCD, no bundled
ST-LINK_gdbserver), but `STM32_Programmer_CLI` has real debugger primitives built in —
`-halt`, `-run`, `-coreReg` (reads `PC/LR/SP/MSP/XPSR/R0-R15` etc.), and
`mode=HOTPLUG` (attach over SWD **without** resetting a running/hung target). That's
enough for this class of bug with no extra tooling:

```sh
# trigger the hang over UART, then, without touching reset:
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -halt -coreReg
```

Findings, in order:
1. `PC`/`LR` on repeated halt/run/halt samples landed in *different*, legitimate places
   each time (`drain_ring`, `on_sample`, `math_phasor_accumulate`, `app_scheduler_run`,
   even I2C driver code) — the CPU was **not** stuck in `HardFault_Handler`'s `while(1)`
   at all. It was executing completely normal code, continuously.
2. `uwTick` (HAL's tick counter, incremented purely by the SysTick ISR, independent of the
   main loop) kept advancing at the normal ~1 ms rate throughout — confirmed by reading it
   twice, 1 s apart.
3. `app_scheduler.c`'s `s_tasks[]` array, read directly out of RAM
   (`STM32_Programmer_CLI -r32 <address of s_tasks> ...`, cross-referenced against
   `arm-none-eabi-nm`'s symbol table), showed `task_displacement`'s `last_run_ms` **frozen**
   at the tick it was first called, while `uwTick` had moved on by hundreds of seconds.
   `task_uart`/`task_api` (which run just before `task_displacement` in the table) were
   frozen one tick *later* than `task_displacement` — i.e. they completed the pass that
   called `task_displacement`, and the scheduler never came back from that one call.
4. Confirmed directly against the actual USART3/DMA2 peripheral registers
   (`-r32` on the `DMA_Channel_TypeDef`/`USART_TypeDef` addresses computed from the CMSIS
   header): the RX DMA ring was receiving bytes correctly at the hardware level (`CNDTR`
   moved by exactly 6 after sending a 6-byte request) — they just sat there, uncounted,
   because `hal_uart_read()`/`svc_uart_update()` (which live *after* `task_displacement`
   in the scheduler table) never ran again to consume them.

**Root cause:** `process_one_cycle()`'s complex-division math (3 float divisions per
cycle, no hardware FPU on this Cortex-M0+) costs more than the ~384 µs a cycle takes to
produce at the 2.6 kHz carrier rate. Compute-only, that was *already* marginal — ~18% of
cycles were being dropped even with nowhere to store the result. Adding the few extra
stores the API snapshot needs tipped the average over budget: `on_sample()` (ISR context)
queued new cycles into the input ring faster than `svc_displacement_update()`'s
`while (s_in_tail != s_in_head)` drain loop could empty it, so that loop's own exit
condition could never become true — a livelock, not a crash. Every earlier bisection
"fix" had really just been removing enough per-cycle cost to duck back under budget, not
fixing a logic bug — which is exactly why the results looked inconsistent.

**The fix** (`Config/config.h`'s `DISPLACEMENT_BATCH_CYCLES`/`DISPLACEMENT_MAX_CYCLES_PER_TICK`
comment has the same writeup in-repo):
1. **Batch `DISPLACEMENT_BATCH_CYCLES` (8) consecutive cycles'** raw I/Q into one coherent
   sum (cheap `int64` adds, unchanged ISR-side accumulation) before running the expensive
   per-batch complex division once — not once per single cycle. Cuts the division-heavy
   work by that factor. Bonus, not just a workaround: longer coherent integration is
   better SNR, not worse.
2. **Bound the drain loop** to at most `DISPLACEMENT_MAX_CYCLES_PER_TICK` (32) raw-cycle
   dequeues per `svc_displacement_update()` call, regardless of backlog — so a future
   transient overload degrades to ordinary graceful drops (`s_input_drop_count`, already
   handled) instead of ever livelocking the scheduler again. Same bounded-pump shape as
   `DISPLAY_PAGES_PER_TICK` / the retired bulk-chunk pump elsewhere in this codebase.

Verified fixed both ways: UART stayed responsive through a **300/300 poll, 242 s (4 min)
continuous soak** with displacement running the whole time, and directly at the register
level — `task_uart`/`task_displacement`'s `last_run_ms` tracked `uwTick` within a tick or
two throughout, instead of freezing. `input_drop`/`output_drop` (`uint16_t`, saturating)
did hit 65535 over that soak — expected at ~632k cycles produced over 4 minutes while also
being hammered with a UART poll every 0.5 s; graceful by design, not a failure. Worth
widening those two counters to `uint32_t` at some point so they're still informative on a
multi-minute run, not urgent.

## Bulk raw-ADC capture restored (2026-09-25)

Retiring the WP8 Bulk raw-ADC-capture path (API v2 category 0x8) alongside
`svc_signal_analysis.c` was overreach, not a necessary consequence of the
port -- it's a used, important bench diagnostic (`PythonTestCode/bulk_adc_csv.py`,
`adc_diag.py`, `adc_signal_analysis.py`), not dead code. Restored by porting the
old module's capture mechanism directly into `svc_displacement.c`'s `on_sample()`:
a `s_cap_active` flag, checked first, makes the callback store raw sign-extended
24-bit codes into a `Config/config.h` `ADC_BULK_SAMPLE_COUNT × 4` buffer and
return immediately -- skipping the phasor accumulation entirely -- exactly the
same "capture mode bypasses the real math" branch the old file had. The two
paths are still mutually exclusive at runtime (never at compile time): `svc_api.c`'s
`dispatch_bulk()` refuses `START_BULK` with `BUSY_EXCLUSIVE` while
`svc_displacement_is_running()`, and refusing to start displacement while a
bulk capture is active is implicit in `drv_ads131m04_start()`'s own busy check.

API surface: `Bulk` (0x8) `START_BULK`/`CANCEL_BULK` resource `0x00`, same wire
shape as before (`[status=OK][page:1][sample:12]×10` chunks). `Raw data` (0x7)
resource `0x00` reverted to its original meaning (register read-back +
`last_capture` samples/drops/elapsed_ms) so the pre-existing Python tooling's
wire format didn't need to change; the WP10 displacement drop-counter
diagnostics that had been squeezed into that same resource moved to their own
new resource, `0x02` (`API2_RES_RAW_DISPLACEMENT_DIAG`), rather than staying
double-booked with the restored bulk stats.

RAM cost: the capture buffer is 6144 × 4 × 3 = 73728 bytes (~72 KiB, ~50% of
the G0B1's 144 KB SRAM) -- same size as before, just living in
`svc_displacement.c` now. Build after restoring: RAM 74.3% / FLASH 29.4%,
zero warnings.

## Current status (fw 0.10.17)

- Channel mapping, calibration store, Commands start/stop, acquisition pipeline: all
  bench-verified.
- Full pipeline verified end-to-end with real data: `disp_ok=1`, plausible small
  (sub-millimeter) delta values, residuals near 0 as expected for a working calibration.
- Deferred, not blocking: the high-rate per-batch stream (nothing drains
  `svc_displacement_pop()` yet); a fresh bench calibration of `gain`/`d0`/`zero_offset`
  (currently nominal/un-calibrated defaults); Sensor 2 (CH0) isn't physically connected on
  this bench yet.
