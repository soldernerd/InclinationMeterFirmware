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

## Phasor diagnostics added (2026-09-25, fw 0.10.27)

Exposed the demod's intermediate I/Q phasors (`BatchSums`, one step upstream of
`delta_mm`/`residual`) for bench diagnosis, at the user's request ("the phasors
definitely, maybe also some other values... real time or at least capture longer time
slots via bulk capture"):

- **Real-time**: Topic groups (0x5) resource `0x02` (`API2_RES_TOPIC_PHASORS`) — the
  latest completed batch's 8 raw phasors bundled into one 32-byte payload, GET +
  SUBSCRIBE, same generic mechanism as every other topic. Chosen over 8 separate
  Measurements resources: an atomic snapshot avoids a torn read across separate polls,
  and Measurements' 16-slot budget was nearly full anyway (`TOPIC_VALUE_MAX_LEN` was
  already provisioned at 32 bytes for exactly this kind of bundle, no size bump needed).
- **Longer time slots**: Bulk (0x8) resource `0x01` (`API2_RES_BULK_PHASORS`) — reuses
  the exact same accumulation/batching pipeline as normal operation (`on_sample()`
  untouched); only `svc_displacement_update()`'s "batch complete" branch forks to store
  a decimated snapshot (every `DISPLACEMENT_PHASOR_LOG_DECIMATION`th batch, 8 by
  default) instead of demodulating. 512 entries at the ~40.7 Hz effective rate spans
  ~12.6 s, versus the raw-ADC capture's ~0.3 s, for the same reason the user gave:
  phasors are far lower data volume per unit time than raw ADC samples. Bench-verified:
  512/512 entries, 0 gap/CRC events end to end.
- A new `svc_displacement_phasor_log_progress()` getter (Raw data 0x7/0x02) lets a host
  poll how far a capture has gotten instead of guessing.

### Timing margin was too thin, not board 2 being defective

Testing on a second REV B board (see memory `two-bench-boards`) surfaced something
board 1 never exposed: **even firmware predating this whole phasor-diagnostics
feature and the LIVE-screen displacement readout** (`git` commit `027aff7`) drops
`svc_displacement.c` input cycles on this board, in bursts — multi-second plateaus of
zero growth interrupted by sudden jumps, averaging roughly 400-700 cycles/s over a 10 s
window. Board 1's original "300/300 poll, 242 s soak, zero drops" verification (the
livelock fix writeup above) never caught this because it was never run on board 2.

The two boards are the same design, differing only by ordinary component/clock
tolerance — the user's framing, and the correct one: two identical boards behaving
differently at a setting this close to its own documented "~18%-marginal" budget (see
the livelock writeup above) means the budget was too thin in general, not that board 2
has a defect. **Fix (2026-09-25, fw 0.10.29)**: pushed the same lever that solved the
original livelock further --
- `DISPLACEMENT_BATCH_CYCLES` 8 → 32 (quarters the division-heavy math's rate; ~81
  updates/s instead of ~325, still ample for a mechanical reading),
- `DISPLACEMENT_RING_DEPTH` 64 → 128 and `DISPLACEMENT_MAX_CYCLES_PER_TICK` 32 → 64
  (doubles the buffering slack against transient scheduler jitter and keeps
  backlog-recovery proportionally as fast).
- `DISPLACEMENT_PHASOR_LOG_DECIMATION` 8 → 2, re-derived to keep the phasor-log
  capture's effective rate/duration at the same ~40.7 Hz / ~12.6 s target as before the
  batch-cycle change (decimation is relative to the batch rate, which just quadrupled).

Bench-verified on board 2: a 30 s soak with the LIVE-screen readout active (the worst
case) dropped ~970 cycles/s, down from ~1500-2000/s pre-hardening — and with the
readout's periodic redraw disabled entirely, drops fall to ~370/s, in line with board
2's pre-existing baseline. A bulk phasor-log capture (512 entries) that previously took
26 s now completes in ~18 s with 0 gap/CRC events, both consistent with meaningfully
less pressure on the pipeline.

Separately, the LIVE-screen displacement readout added in the prior session turn (fw
0.10.22) was ALSO independently making things worse (roughly 3-4x on top of the
baseline above) regardless of its refresh interval (250 ms, 1000 ms, and 2000 ms all
measured within roughly the same range of each other) — traced to
`format_displacement_mm()`'s float math running once per rendered *band* (u8g2 page
mode calls `draw_live_screen()` ~15x per redraw) instead of once per redraw. Moved the
formatting into `snapshot_capture()` (`App/app_display.c`, fw 0.10.27) and slowed the
refresh interval to 2000 ms, which together measurably reduce the readout's own
contribution but do not eliminate it -- the redraw's banded rasterization + Sharp LCD
DMA blit still costs something that doesn't scale down linearly with refresh interval,
and isn't fully root-caused. Not blocking: the margin-hardening above is the change
that actually restored most of the headroom; the display's residual cost is a smaller,
secondary effect worth a closer look if the readout's refresh rate ever needs to go
back up.

## Zero calibration (180-degree reversal test, 2026-09-25, fw 0.10.31)

Standard precision-level calibration procedure, added on request: place the instrument,
trigger step 1; physically rotate it 180 degrees in place, trigger step 2; the instrument
is then zeroed. Config/config.h's "Displacement zero calibration" comment has the full
derivation -- in short, a real surface tilt contributes with opposite sign to each step's
reading while the instrument's own zero error doesn't, so averaging the two steps cancels
the (unknown) surface tilt and isolates the (wanted) zero error:
```
step1 = surface_tilt + zero_error
step2 = -surface_tilt + zero_error
zero_error = (step1 + step2) / 2
```
Applied independently per sensor head (S1, S2 each have their own `disp_s1/s2_zero_offset_um`
calibration constant already) -- one 180-degree flip of the whole instrument zeroes both
sensors in a single two-step run, since both are rigidly mounted in the same housing and
both see the same physical rotation.

**Entirely per-instrument by construction**: the whole procedure only ever reads/writes
*this* device's own `g_device_settings` and *its own* EEPROM (Services/svc_storage.c) --
there is no shared or global calibration state, so running this on one physical unit can
never affect another's calibration. Confirmed with the user as an explicit requirement,
not just incidentally true.

**API**: Commands (0x1) resource `0x06` (`API2_RES_CMD_ZERO_CAL`), EXECUTE, 1-byte
payload: `0x00` cancel, `0x01` step 1, `0x02` step 2. Each call just arms/cancels a step
and acks immediately -- averaging `DISPLACEMENT_ZERO_CAL_SAMPLES` (128, ~1.6 s at the
default batch rate) batches happens over the following ticks inside
`svc_displacement.c`'s `process_one_batch()`, the same place `delta1_mm`/`delta2_mm`
themselves get computed. Progress is pollable via Raw data (0x7) resource `0x03`
(phase/progress/target); once step 2 finishes, `svc_api.c`'s `svc_api_update()` applies
and persists the result within the same tick (rounded to the nearest micrometer, clamped
to the existing Calibrations `ZERO_OFFSET_UM` bounds of ±5000) -- a host reading
Calibrations 0x2 resources `0x03`/`0x06` afterward sees the new values directly, no
separate "commit" step needed. Requires the demod already running (Commands
`API2_RES_CMD_DISPLACEMENT`); `svc_displacement_stop()` or a fresh `start()` cancels an
in-progress run rather than leaving stale step-1 data around.

Bench-verified on board 2 (mechanism only -- see caveat below): step 1 and step 2 both
completed in ~1s with correct progress reporting; all edge cases correctly rejected
(step 1 while not running, step 2 before step 1, an invalid action byte, cancel while
already idle); offsets went from `(0, 0)` µm to `(-1, -1)` µm and `disp1_delta_mm`/
`disp2_delta_mm` shifted by the mathematically correct amount and direction; the new
offsets persisted across a stop and a fresh GET (confirmed written to EEPROM, not just
RAM). A rounding bug was caught and fixed during this verification: the mm-to-µm
conversion originally truncated toward zero instead of rounding to nearest, silently
discarding any correction under 1 µm.

**Caveat on the bench verification above**: both steps were triggered at the SAME
physical orientation (no actual 180-degree flip was performed -- this session has no way
to physically manipulate the instrument). That exercises the state machine, API,
persistence, and math direction correctly, but is NOT a real calibration run --
performing an actual flip between step 1 and step 2 is the real end-to-end test still
needed on real hardware.

## Current status (fw 0.10.31)

- Channel mapping, calibration store, Commands start/stop, acquisition pipeline: all
  bench-verified.
- Full pipeline verified end-to-end with real data on two boards: `disp_ok=1`, plausible
  small (sub-millimeter) delta values, residuals near 0 as expected for a working
  calibration. Board 2 has both S1 and S2 physically connected (board 1 only has S1).
- Bulk raw-ADC capture and bulk phasor-log capture both bench-verified, including their
  mutual exclusivity with the real-time demod and with each other.
- Timing margin hardened (`DISPLACEMENT_BATCH_CYCLES`/`_RING_DEPTH`/
  `_MAX_CYCLES_PER_TICK`, see above) after board 2 exposed the original tuning as too
  thin in general, not board-specific.
- Zero calibration (180-degree reversal test) implemented and API-accessible, mechanism
  bench-verified -- see above. Still needs a real physical-flip test.
- Deferred, not blocking: the high-rate per-cycle stream (nothing drains
  `svc_displacement_pop()` yet); a fresh bench calibration of `gain`/`d0`/`zero_offset`
  (currently nominal/un-calibrated defaults — the bulk-capture signal-quality pass found
  the nominal S-channel gain of 10x doesn't match either board's actual hardware, real
  gain is closer to ~0.13x); the LIVE-screen readout's own residual, not-fully-root-caused
  rendering cost noted above; a real physical-flip zero-cal test.
