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

## Auto-start + LIVE screen redesign (2026-09-26, fw 0.10.32)

User feedback after physically looking at the instrument's screen: displacement showed
"(not running)" (it had to be started over the API, with no way to do that locally), and
the LIVE screen gave temperature/battery the prominent big-font treatment while burying
displacement in one small line at the bottom.

- **Auto-start**: `svc_displacement_start()` is now called once at the end of
  `Core/Src/main.c`'s setup (comms/RTC/power/scheduler table all already up), not gated
  behind an API command anymore. The original "toggle over the API" design predated this
  session's margin-hardening work; with that headroom restored, and given the instrument's
  whole point is to show a reading without a host connected, requiring an API call first
  was the wrong default. Bench-confirmed: `disp_ok=1` immediately after a fresh boot, no
  start command sent.
- **LIVE screen**: displacement (S1/S2) now gets the big `logisoso24` font that
  temperature/battery used to have; temperature/SoC/voltage moved to one small side-info
  line. Same proven Y coordinates, just swapped content -- not visually confirmed from this
  session (no way to see the physical panel), verified only via the underlying data path.
- **Battery voltage** (unrelated bug, same feedback pass): board 2 read an "obviously
  wrong" 11.80 V. `EEPROM_BATTERY_SETTINGS_VERSION` bumped 0x0004->0x0005 so the R6/R9
  divider-swap fix from 2026-09-24 (which only ever patched board 1's live EEPROM, never
  propagated via a version bump) finally reaches every already-provisioned board. Bench-
  confirmed: `vbat_scale_den` 33->100 automatically after reflashing, `battery_mv`
  11792->3884 (a plausible single-cell reading).

## Sensitivity bumped 1000x, coarse fix (2026-09-26, fw 0.10.33)

User feedback after using the auto-started, redesigned LIVE screen: "the instruments
function, their sensitivity is way too low. Without even calibrating, it is probably
1000 times too low, maybe even more. increase sensitivity by 1000x and we can calibrate
later but this should get us in the right ballpark."

`DEFAULT_DISP_S1_D0_UM`/`DEFAULT_DISP_S2_D0_UM` bumped 100 -> **100000** (0.1 mm -> 100 mm
nominal). `compute_sensor_delta()`'s `delta_mm = 2*d0_mm*(x-0.5) - zero_offset_mm` makes
`d0` a **pure linear scale factor on the final output only** -- zero effect on `x`,
`residual`, or the degenerate-denominator check, unlike `gain` (which sits inside `x`'s
own computation and also feeds the shared `A-B` reciprocal). That's exactly why `d0`, not
`gain`, is the lever here: a clean, isolated multiplier, not a change tangled up in the
demod math. **This is not a claim the sensor's real mechanical air gap is 100 mm** -- `d0`
is standing in for the still-missing real gain calibration (the bulk-capture
signal-quality pass separately found the nominal `gain=10` doesn't match either board's
actual hardware, real gain closer to ~0.13 -- a ~77x mismatch on its own, roughly the
right order of magnitude once the S-channel's real-vs-assumed attenuation is folded in
too). `EEPROM_DISPLACEMENT_SETTINGS_VERSION` bumped 0x0001 -> 0x0002 alongside it so this
actually reaches already-provisioned boards (the exact lesson from the battery-scale bug
above, applied immediately this time) -- confirmed this also resets `zero_offset_um` back
to 0 on reflash, which is correct here since the old zero-cal result was fit to the old
1000x-smaller scale and would otherwise be silently wrong at the new one.

Bench-verified on board 2: `disp_s1/s2_d0_um` read back `100000` after reflashing (no
manual SET needed), `zero_offset_um` reset to `0`, and `disp1/disp2_delta_mm` jumped from
sub-micrometer noise-floor values (~0.0007 mm) to clearly-scaled mm-range values
(~0.21-0.24 mm) for the same physical (unmoved) instrument -- the expected order-of-
magnitude jump. Exact ratio varies run-to-run (expected, real sensor noise plus the
zero-offset reset changing the additive term), not a precise 1000.000x -- consistent with
the user's own framing ("in the right ballpark," not a real calibration).

**Zero calibration should be re-run** after this change -- any previous 180-degree
reversal result was computed against the old (1000x smaller) scale and no longer applies
(though the EEPROM version bump above already discarded it automatically).

## A further 7x, from a real physical check (2026-09-26, fw 0.10.34)

Same day: the user ran an actual physical check against the 1000x-sensitivity build --
a single 80 g/m^2 paper sheet (~100 um caliper, per its grammage/density) as a known shim
under one end of the instrument over a known baseline, comparing the resulting reading
against the expected displacement -- and reported "you can multiply sensitivity by 7x
from here and it's about right."

`DEFAULT_DISP_S1_D0_UM`/`DEFAULT_DISP_S2_D0_UM` bumped again, 100000 -> **700000**
(~7000x total from the original 100/0.1 mm). `EEPROM_DISPLACEMENT_SETTINGS_VERSION`
bumped again too, 0x0002 -> 0x0003 -- same propagation reasoning as before, applied on
every default change, not just the first one. This is a more trustworthy number than the
initial 1000x guess (an actual physical measurement, not a component-tolerance estimate),
but it's still a coarse per-board sanity check, not a real multi-point calibration against
a certified reference.

Bench-verified on board 2: `d0` read back `700000` after reflashing, offsets reset to `0`
again, `disp1/disp2_delta_mm` reads ~1.29-1.70 mm for the same physical (unmoved)
instrument -- roughly 7x the previous (~0.21-0.24 mm) reading, consistent with the change
(exact ratio varies run-to-run for the same real-noise reasons as the first bump).

## Zero calibration added to the local menu (2026-09-26, fw 0.10.35)

User asked where the zeroing routine was in the menu structure -- it wasn't: WP10's
zero-cal shipped API-only (Commands 0x1/0x06), even though the described workflow
("user places the instrument and starts step 1... turns instrument 180 degrees and
triggers step 2") is clearly a local, hands-on operation. Added a `UI_SETTING_ZERO_CAL`
action row to the SETTINGS screen (`App/app_ui.h`/`.c`, `App/app_display.c`), between
"Force charge" and "Reboot to DFU", following the exact existing action-row pattern
(first RIGHT press shows a confirm prompt, second RIGHT press fires). One press starts
whichever step `svc_displacement_zero_cal_get_phase()` says is next -- step 1 normally,
step 2 once step 1 has finished. Unlike the other action rows, this one shows **live
progress text** (`[step 1: 43/128]`, then `[flip 180, RIGHT]` once step 1 finishes, then
`[step 2: 12/128]`) computed fresh every redraw regardless of cursor position, since the
averaging runs in the background over the following ~1-2 s independent of what the UI is
doing. Shares the exact same `svc_displacement_zero_cal_*()` calls the API path already
used and bench-verified -- confirmed no regression there after adding the UI code.

## Higher sensitivity/SNR via batch size + moving average, and a display tear fix (2026-09-26, fw 0.10.40)

Two independent, user-requested changes, plus a third finding surfaced by bench-testing them:

**1. `DISPLACEMENT_BATCH_CYCLES` 32 -> 128 + a new post-division moving average
(`DISPLACEMENT_MA_SAMPLES = 4`, `Services/svc_displacement.c`'s `ma_apply()`).**
User's own math: at 32 cycles/batch the division rate was ~81.4 Hz
(2604.167/32); at 128 it's ~20.3 Hz -- still "20 samples" as the user put it,
and correct. This cuts the division-heavy per-batch work another 4x (on top
of the 2026-09-25 hardening) and is a real SNR win on its own: √4 = 2x
(~6 dB) from the longer coherent integration, stacking with the new moving
average's own √4 = 2x (~6 dB) for roughly 4x (~12 dB) total. The MA is a
cheap boxcar over the last 4 batches' already-divided `delta_mm`, applied
inside `process_one_batch()` before `s_delta1_mm`/`s_delta2_mm` are updated
-- transparent to every consumer (LIVE screen, API Measurements) via the
existing getters. Zero-cal deliberately bypasses it (fed the pre-MA raw
value) since it already does its own, much longer, independent averaging.
`DISPLACEMENT_ZERO_CAL_SAMPLES` was rescaled 128 -> 32 alongside the batch
bump so its wall-clock duration per step (~1.6 s) and total raw-cycle
integration depth (4096) are unchanged, not re-tuned.

**2. LIVE screen tear fix ("screen sometimes updates in halves").** Not a
framebuffer/double-buffering problem -- the frame was already effectively
double-buffered (15 u8g2 page-mode bands accumulate into one off-screen RAM
buffer across several scheduler ticks, then one atomic DMA blit at the end,
`docs/display_page_render.md`). The real bug: `draw_active_screen()` read
`g_system_state.battery_low` and `g_ui_state.current_screen` LIVE, fresh
every band, instead of from the `s_last` snapshot the numeric fields already
freeze at render start -- exactly the gap that doc's own "Not done" section
flagged. If the screen selection (or low-battery overlay) changed mid-render,
different bands landed in the same buffer from different states before the
one flush, so the single atomic panel update visibly mixed two screens. Fixed
by adding `battery_low` to `DisplaySnapshot` and routing `draw_active_screen()`
and the SETTINGS screen's cursor/edit-highlight (`settings_cursor`/
`settings_editing`/`edit_value`) through `s_last` instead of the live globals.
Contained to `App/app_display.c`.

**3. A third, pre-existing, unrelated bug found while bench-verifying #1: see
[[wp10-redraw-cost-bug]] (memory) / `App/app_display.c`'s
`LIVE_DISPLACEMENT_REFRESH_MS` comment.** The user also asked to bring the
LIVE-screen refresh rate back down (250 ms, from 2000 ms) now that #1 frees
CPU margin. A fine-grained (200 ms) drop-count sampler -- finer than the
coarse before/after snapshots the 2026-09-25 investigation used -- found a
real ~500-cycle (~190 ms) `input_drop_count` burst every single
`LIVE_DISPLACEMENT_REFRESH_MS` period, present even at the ORIGINAL 2000 ms /
32-cycle settings (i.e. this was never actually fixed, just not measured
finely enough to see). `DISPLACEMENT_BATCH_CYCLES=128` made no difference to
burst size, ruling out division cost and pointing at the banded redraw itself
(likely the LIVE screen's large `logisoso24` S1/S2 glyphs, not root-caused
further this session). Refresh rate was therefore left at 2000 ms rather than
lowered -- every interval tested (250/1000/2000/30000 ms) drops the same
~500 cycles per redraw, so a shorter period is strictly worse, not better,
until the real per-redraw cost is found and fixed. This is an open item, not
resolved by this session's work.

Bench method note: isolated the redraw's contribution from separate BLE/LED
jitter by disabling those via the `powertest` (Commands 0x03) mask -- but
that mask only cuts physical `DISP_ON`/`VCOM` pins for `PWR_DISPLAY`, NOT the
render pipeline itself (`task_display` still fully renders + blits every
tick regardless), so it's useless for isolating rendering cost specifically.

## PGA gain on S1/S2, and clip/amplitude-fault detection (2026-09-26, fw 0.10.41)

**PGA gain.** User's question: "we need a measurement range of ±1mm/m, what gain
setting can we afford?" A fresh bulk capture (not the stale WP8-era doc numbers, which
turned out to be superseded -- the ~170mV "not ground-referenced" DC offset noted there
is now only ~7mV, whatever changed since) gave the real answer: at the ADS131M04's
default PGA=1 (full scale 2.4V), S1/S2 sit at only ~52-58mV peak (+~7mV DC offset) --
~2.5% of full scale -- while A/B sit at ~1250mV (~52% of full scale, already near their
own ceiling, so left untouched). Gain=16 (full scale 150mV) uses ~43% of the new,
smaller full scale -- comfortable margin, and a real 16x resolution improvement.
Gain=32 was rejected: ~87% of a 75mV full scale at the same measured signal, too tight
given that measurement was taken at whatever tilt the bench happened to be at, not a
certified ±1mm/m reference.

Set via `Drivers_App/drv_ads131m04.c`'s `GAIN1_REG_VALUE = 0x4004` (PGAGAIN0=PGAGAIN3=4
i.e. gain=16 on ch0=S2/ch3=S1; PGAGAIN1/2=0 i.e. gain=1 on ch1=B/ch2=A, unchanged).
Bench-confirmed with a before/after bulk capture: S1/S2 peak-to-peak scaled by almost
exactly 16.00x (114.96mV->1842.28mV on S2, 104.66mV->1673.27mV on S1), A/B unchanged
(2510.70mV/2484.47mV -> 2509.93mV/2484.71mV, within capture-to-capture noise) -- the
gain change landed exactly as intended, on exactly the two channels intended.

**Software gain compensation.** The ADC's own PGA is a SEPARATE, independent gain stage
from `disp_s1/s2_gain_milli` (an external analog pre-amp calibration constant used in
`compute_sensor_delta()`'s `x=(S/k-B)/(A-B)`, `k=atten*gain`). Since A/B stay unscaled
while S1/S2 now read back 16x larger raw codes, `DEFAULT_DISP_S1/S2_GAIN_MILLI` were
bumped 16x too (10000 -> 160000, `Config/config.h`) to cancel the ADC's own amplification
back out of the ratio math -- otherwise `x` (and `delta_mm`) would silently read 16x too
small. `EEPROM_DISPLACEMENT_SETTINGS_VERSION` bumped again (0x0003 -> 0x0004) so this
propagates to board 2's already-provisioned EEPROM, same reasoning as every earlier
bump this session.

**Clip / amplitude-fault detection**, added at the user's request ("clipped readings or
calculated amplitude>theoretical maximum should generate an error") specifically because
raising PGA gain is exactly the kind of change that could introduce clipping. Two
DIFFERENT checks, despite sounding similar -- see `Services/svc_displacement.h`'s getter
comment for the full reasoning:
- **`clip_count`** (`svc_displacement_get_clip_count()`) -- a real clipping detector: any
  raw ADC code (any of the 4 channels) within ~99% of the ADS131M04's own digital rail
  (`ADS131M04_CLIP_THRESHOLD`, `Drivers_App/drv_ads131m04.h`'s `ADS131M04_CODE_MAX`),
  checked per raw sample in `on_sample()`.
- **`amplitude_fault_count`** (`svc_displacement_get_amplitude_fault_count()`) -- NOT a
  second clipping detector. A completed batch's raw phasor magnitude
  (`sqrt(i^2+q^2)`) exceeding the highest value a genuinely full-scale, UNDISTORTED
  sinusoid at the matched carrier frequency could ever produce
  (`DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG = DISPLACEMENT_BATCH_CYCLES * 65536 *
  ADS131M04_CODE_MAX`, derived from `math_phasor.c`'s 8-point matched-filter sum). Real
  clipping flattens the waveform and can only ever REDUCE this value relative to a clean
  sinusoid (harmonic energy leaks out of the fundamental bin) -- so exceeding this ceiling
  means something else: a gain/scale mismatch (exactly the software-compensation step
  above, if it were wrong), corrupted data, or an accumulation bug. Both are saturating
  counts, reset at start()/init(), logged once (edge-triggered) via
  `svc_displacement_check_integrity()`, and surfaced over the API on Raw data (0x7) GET
  `API2_RES_RAW_DISPLACEMENT_DIAG` (extended from 9 to 13 bytes).

Bench-confirmed: `amplitude_fault_count` stayed 0 throughout (as expected -- the check
is a sanity assertion that should never fire under normal operation, not a tuned
threshold). `clip_count` caught a real, one-time event: 291 clipped samples right at one
particular `svc_displacement_start()`, static (non-growing) afterward and disp_ok=true
throughout -- almost certainly a startup transient (analogous to WP7's documented
`fOUT/8` startup glitch) before the signal chain settled, not a steady-state margin
problem. A second restart produced clip_count=0, confirming it's transient/probabilistic
rather than deterministic. Steady-state signal sits at ~36-40% of the ADC's raw code full
scale on S1/S2 (measured directly from the bulk capture's raw min/max, gain-independent)
-- comfortably not clipping, consistent with the ~43%-of-FSR estimate above.

**Still open:** this is a proxy measurement (whatever tilt the bench happens to be at),
not a calibrated ±1mm/m reference -- a real answer to "does ±1mm/m specifically fit"
needs either the sensor's true mechanical sensitivity or a controlled reference tilt.

## Raw (pre-moving-average) displacement stream (2026-09-26, fw 0.10.42)

The 10-minute unattended monitor above was done by *polling* Measurements
(0x4) GET repeatedly -- ~1.6s/sample in practice (UART round-trip overhead
per request, not the 0.4s intended), badly undersampling a ~20.3 Hz source
(config.h's `DISPLACEMENT_BATCH_CYCLES`/complex division rate) and, worse,
reading `svc_displacement_get_delta1/2_mm()`'s POST-moving-average value
(`DISPLACEMENT_MA_SAMPLES`), not the raw per-batch number. User correctly
pointed out both problems: this needs a real subscribable stream of the
pre-MA value, and the API v2 architecture already has exactly the
mechanism for it (Topic groups, category 0x5 -- the phasors topic already
does this for the raw I/Q phasors).

Added Topic groups resource **0x03 "Raw displacement"**
(`API2_RES_TOPIC_RAW_DISPLACEMENT`, `svc_api.h`/`.c`): 16 bytes --
`delta1_mm_raw`, `residual1`, `delta2_mm_raw`, `residual2` -- built from
two new getters, `svc_displacement_get_delta1/2_mm_raw()`
(`Services/svc_displacement.c`), populated in `process_one_batch()`
*before* `ma_apply()` runs. Subscribe at the
`API2_MEASUREMENT_MIN_INTERVAL_MS` floor (50 ms) to track the ~49.2 ms
batch period essentially 1:1 -- this is the existing polled-push
subscription model (`svc_api_topic_subscriptions_update()`, called every
scheduler tick), not a new streaming mechanism; it just needed a topic
that exposes the pre-MA value, which didn't exist before.

Fits directly into the existing `API2_TOPIC_SLOTS = 4` array (3 topics
were already defined, one free slot) -- no slot-table resize needed.

**Bench-confirmed, fw 0.10.42:** a 15s smoke test before the full 10-minute
run got 260 samples (~17.3 Hz effective, host-loop-limited, not
device-limited) with **zero sequence gaps** (`issue_seq`, the push
counter, incremented by exactly 1 every time) -- the transport isn't
dropping frames at this rate. Pre-MA noise (raw std ~0.008mm) is visibly
higher than the post-MA figure from the earlier polled measurement
(~0.0022mm) as expected from a 4-sample boxcar's ~2x noise reduction.

`PythonTestCode/apiv2.py` got `TOPIC_RAW_DISPLACEMENT` + `decode_topic_raw_displacement()`.

## Per-batch quality flag and triggered precision measurement (2026-09-26, fw 0.10.43)

Two related, user-requested features, both directly built on this session's noise
investigation and the raw streaming capability that made it possible to verify them.

**Quality flag.** User asked whether the discrete "jumps" found in the noise
investigation correlate with the residual (Im(x)) enough to distinguish good readings
from bad ones. Checked directly against the two already-collected 10-minute streaming
datasets (no new capture needed): residual steps run **~4-4.6x bigger** at the exact
same batches where delta_mm jumps, consistently across the noisy channel, the quiet
channel, both before and after the sensor swap. Implemented as
`svc_displacement_get_quality1/2_ok()` (`Services/svc_displacement.c`): a per-channel
EWMA baseline of `|residual step|` (`DISPLACEMENT_QUALITY_EWMA_SAMPLES = 32` batches,
~1.6s), updated only on already-good batches (so a sustained noisy patch can't inflate
its own bar), flagging a batch bad when its residual step exceeds
`DISPLACEMENT_QUALITY_BAD_MULTIPLE = 3` times that baseline -- real margin below the
observed ~4x ratio. Surfaced on Topic groups `0x03` (extended 16 -> 18 bytes:
`quality1_ok`, `quality2_ok`).

**Triggered precision measurement.** User's actual use case: not the continuous live
readout, but "trigger via the API, device takes the time it needs, reports one reliable
value," suggesting averaging 64 known-good divisions, "ideally" completing in ~2s.
Pointed out the tension in that framing directly: at the current ~20.3 Hz batch rate, 64
samples takes **~3.15s minimum even with zero discards** -- already past "~2s" before
any bad-batch filtering. Resolved as bounded best-effort:
`DISPLACEMENT_PRECISION_TARGET_SAMPLES = 64` (a real sqrt(64)=8x SNR improvement) with a
hard `DISPLACEMENT_PRECISION_TIMEOUT_MS = 4000` ceiling, so a triggered measurement can
never block indefinitely -- the result always reports how many quality-good batches were
actually averaged per sensor, so a caller knows the achieved confidence rather than a
silent shortfall.

New API: Commands `0x07` (`API2_RES_CMD_PRECISION_MEASURE`, 1-byte payload: 0=start,
1=cancel) arms/cancels a run; Raw data `0x04` (`API2_RES_RAW_PRECISION_STATUS`, 20 bytes:
phase, target, per-sensor counts, elapsed_ms, timed_out, the two averaged results) polls
progress and reads the result once done. Unlike zero-cal, there's no EEPROM write and
therefore no "consume" step -- the result is a plain re-readable GET once `DONE`.
Mutually exclusive with an in-progress zero-cal (both are one-shot consumers of the same
batch stream); `svc_displacement_stop()`/a fresh `start()` cancel an in-progress run, same
reasoning as zero-cal's own cancel-on-stop.

**Bench-verified, fw 0.10.43, board 2:** a real triggered run took **4.73s wall-clock**
(not the ~3.15s "clean-channel" estimate) and finished via the timeout path (60/64 and
58/64 samples, `timed_out=true`) rather than reaching the full target. Both channels'
counts climbed in near-lockstep (2,14,23,30,36,42,51,58,60 vs 2,14,23,29,35,40,49,56,58)
-- similarly low discard rates on both, not a quality-flag problem. The slower-than-
predicted throughput and the `elapsed_ms=4390` (390ms past the nominal 4000ms timeout)
both point at the still-open [[wp10-redraw-cost-bug]]: the LIVE screen's periodic ~190ms
scheduler stall (every `LIVE_DISPLACEMENT_REFRESH_MS`) eats into batch-processing time
and delays the timeout check itself when one lands nearby -- another concrete reason
that bug is worth fixing eventually, not just a display cosmetic issue. The final
averaged result (2.7545mm / 2.9891mm) matched the live post-MA readings taken just
before triggering (2.7530mm / 2.9932mm) closely, a good sanity check that the averaging
itself is correct. Cancel verified separately: mid-run cancel returns cleanly to
`IDLE`/`count=0`.

## Scheduler-gap investigation: the redraw-cost bug, quantified (2026-09-26, fw 0.10.45-48)

Follow-up to the precision-measurement timing anomaly (4.73s wall-clock instead of the
~3.15s clean-channel estimate, and a 390ms timeout overshoot) -- the user asked for the
underlying timing/slowdown issue to be properly investigated, not just flagged again.

**New instrumentation** (fw 0.10.45-46): `svc_displacement_get_max_update_gap()`
(`Services/svc_displacement.c`) tracks the longest gap between consecutive
`svc_displacement_update()` calls since the last start, plus a frequency count of gaps
`>= DISPLACEMENT_GAP_WARN_THRESHOLD_MS` (40ms, config.h). Rationale: the driver-level
acquisition (`drv_ads131m04`'s own `frame_deficit`/`ring_overflow`) has repeatedly
measured perfectly healthy while `svc_displacement`'s own input ring still drops --
this localizes the cause to scheduler latency, not the ADC/DMA layer, and this
instrumentation quantifies exactly how bad that latency gets. Surfaced on Raw data
`0x7/0x02` (extended 13 -> 19 -> 21 bytes across the two additions).

**First surprise:** the max gap at the default 2000ms/32-cycle-vs-128-cycle settings was
only ~76ms, not the ~190ms the ORIGINAL (2026-09-25/26 earlier) indirect measurement
(drop-count deltas around a redraw) implied. But ~69 such gaps happened in 15 seconds
(~4.6/s) -- far more often than "once per redraw" if redraws only fire every 2000ms.
Resolved: a redraw is 15 bands / `DISPLAY_PAGES_PER_TICK`=3 = 5 ticks, and each tick's
3-band chunk itself costs ~40-76ms -- 5 such ticks per redraw reproduces the earlier
~190-380ms figure; it was never one monolithic stall, it's 5 smaller ones.

**Hypothesis 1, tested and NOT the dominant cause:** ordinary battery-ADC noise (observed
3754-3758mV at rest -- all four values round to the identical displayed "3.75V") was
retriggering a full redraw via `snapshot_changed()`'s bare `!=` comparison on
`battery_mv`, no deadband, roughly once/second (`task_battery_ms`'s 1Hz update rate).
Fixed anyway (real, if smaller, waste): `App/app_display.c`'s `DisplaySnapshot` now
stores `battery_mv_bucket` (quantized to the ~10mV `format_volts()` actually displays)
instead of the raw 1mV value. Bench-verified this fix alone did NOT reduce gap
frequency (86 gaps/15s after the fix vs 69 before, within run-to-run noise) -- ruled out
as the dominant trigger, though the fix is still correct and kept (redraws for an
invisible sub-10mV change were always pure waste regardless of their share of the total).

**Hypothesis 2, confirmed dominant:** temporarily rebuilding with
`LIVE_DISPLACEMENT_REFRESH_MS` at 300000ms (redraws effectively never fire) cut the gap
count ~62% (69->26 per 15s) and the drop rate ~64% (522->188/s). The redraw itself --
regardless of exactly what triggers it -- really is the majority contributor, confirming
(with real instrumentation this time, not indirect inference) what the 2026-09-25/26
investigations suspected but couldn't fully prove.

**Secondary contributor found:** BLE+LEDs off (via the `powertest` mask, stacked on top
of the disabled redraw) cut the remainder a further ~33% (188->126/s).

**Still open:** ~126/s of drops remain unattributed even with the redraw disabled and
BLE/LEDs off. `task_uart`/`task_api` (both run every scheduler tick, unconditionally)
and BME280's disconnected-retry cost (`task_bme280`, 1Hz, the sensor isn't connected on
this bench) are the next suspects -- not yet individually isolated. The actual fix for
the redraw's own per-band cost is also still not found; the LIVE screen's large
`logisoso24` S1/S2 glyphs remain the prime suspect (unchanged from the earlier
writeup), but this session didn't get as far as per-glyph profiling.
`LIVE_DISPLACEMENT_REFRESH_MS` stays at 2000ms (restored after the bisection) -- see
that constant's own comment for why a shorter interval would make things worse, not
better, until the per-band cost itself is fixed.

## 2-hour drift + charging comparison, and an "End charging" command (2026-09-27, fw 0.10.49)

User request: a long (1-2h) unattended drift measurement once self-heating stabilizes,
plus a comparison of noise with USB charging on vs off, plus a challenge to the earlier
"shared electronics drift" explanation -- the ratiometric math should cancel any shared
amplitude/reference drift, so what temperature-dependent term could survive that?

**Theoretical point conceded and refined:** the analytical cancellation
(`x = (S/k-B)/(A-B)`, any common multiplicative factor on A/B/S cancels exactly) is
correct. What does NOT cancel: `k = atten*gain` is a fixed SOFTWARE constant, not
measured in real time -- a real, asymmetric drift specific to the S-channel's own gain
path (external amp, the sensor's internal electronics, or the ADC's per-channel PGA),
not shared with the A/B path (only a passive attenuator), would show up uncancelled.
This is consistent with "45-year-old sensor electronics only powered when the sine
excitation is on."

**Added `EXECUTE 0x1/0x08` "End charging"** (`svc_battery_cancel_force_charge()`) --
force-charge previously had no way back off except reaching full or physically removing
USB, a real gap hit while setting up this exact test (a smoke-test force-charge trigger
left the board charging with no way to stop it short of unplugging the cable).

**2-hour run** (`Topic 0x5/0x03` streamed continuously at ~18 Hz, 129151 samples, ZERO
sequence gaps over the full 2 hours): 60 min natural (not charging) baseline, then
`EXECUTE 0x1/0x02` (force charge) for the remaining 60 min, USB already connected
throughout (battery was at 98% SoC, charged overnight -- a caveat: forced charging on a
near-full battery draws less current/heat than mid-charge would).

**Noise while charging -- clear, actionable answer:**

| | baseline (not charging) | charging (forced) | change |
|---|---|---|---|
| S1 step std | 0.00293mm | 0.00387mm | +32% |
| S2 step std | 0.00526mm | 0.00729mm | +39% |
| S1 jumps >0.02mm | 0.028% | 0.269% | ~9.6x |
| S2 jumps >0.02mm | 0.813% | 2.370% | ~2.9x |
| quality1/2_ok=0 rate | 2.79% / 3.22% | 3.18% / 4.13% | modestly higher |

Charging measurably and substantially increases noise on both channels, at every
threshold checked. **Recommendation: avoid taking precision measurements while
charging** -- a real, simple, actionable finding independent of the drift question below.

**Drift is real, but does NOT reduce to a simple temperature-proportional model** --
this is where the picture gets more complicated than the earlier 10-minute session
suggested:
- Net drift over each 60-min phase: baseline S1 +6.8um / S2 +5.1um; charging S1 -22.6um
  / S2 -25.1um -- charging shows ~3-4x more total drift, AND in the opposite direction
  from the baseline phase's trend.
- Live monitoring during the first ~30 min of charging showed a genuine non-monotonic
  wobble (down ~6um, up ~6um, up ~3um, down ~5um, down ~7um) while temperature was
  still gently rising then plateauing -- not a clean single-time-constant settling curve.
- **Correlation with the onboard temperature sensor FLIPS SIGN between phases**:
  baseline S1/S2 vs temp r=-0.63/-0.55; charging r=+0.43/+0.42. A real, simple thermal
  coefficient should keep the same sign regardless of phase -- this rules out the
  earlier session's clean single linear "14-15um/degC" story as the whole picture; that
  finding likely captured one particular monotonic segment of a more complex process,
  not a universal coefficient.
- **S1 and S2 stay extremely tightly correlated in BOTH phases** (r=0.985 baseline,
  r=0.999 charging) -- if anything, TIGHTER than the earlier 10-minute session's 0.997.
  This is itself informative: two independently-aging 45-year-old sensors, each with
  their own internal electronics drifting for their own reasons, would not be expected
  to correlate this tightly by coincidence. This tilts AWAY from "each sensor's own old
  electronics, coincidentally similar" and toward something genuinely SHARED -- either
  the asymmetric-gain-path mechanism above, or a shared MECHANICAL effect (e.g. a common
  mounting bracket/plate flexing with temperature, physically tilting both sensor zero
  points together) that would explain the correlation without violating the
  amplitude-cancellation argument at all, since it's a real physical effect, not an
  electrical artifact that should have cancelled.

**Still unresolved:** which of (asymmetric S-channel gain drift) vs (shared mechanical
mounting flex) vs (something else) is the actual mechanism -- this session couldn't
distinguish them. A follow-up that would help: watch the raw A/B exciter phasor
amplitude (Topic 0x5/0x02) during a similar thermal transient -- a shared mechanical
tilt would show up as a real x/delta change (indistinguishable from tilt, by
construction), while an asymmetric S-channel gain drift would show up as S's phasor
magnitude changing without a matching change in the physical setup.

## Current status (fw 0.10.49)

- Channel mapping, calibration store, Commands start/stop, acquisition pipeline: all
  bench-verified. Displacement now auto-starts at boot (see above) instead of requiring
  an API command.
- Full pipeline verified end-to-end with real data on two boards: `disp_ok=1`. Sensitivity
  bumped ~7000x total from the original nominal (1000x initial guess + a 7x correction
  from a real paper-shim check, see above) -- still a coarse per-board sanity check, not a
  real multi-point calibration against a certified reference. Residuals near 0 as expected
  for a working calibration. Board 2 has both S1 and S2 physically connected (board 1 only
  has S1).
- S1/S2 ADC PGA now 16 (was 1), with the matching 16x software `gain` compensation
  (see above) -- a real ~16x resolution improvement, bench-confirmed exact. Clip and
  amplitude-fault detection added alongside it; both bench-verified (a real transient
  clip caught once, zero false-positive amplitude faults).
- Bulk raw-ADC capture and bulk phasor-log capture both bench-verified, including their
  mutual exclusivity with the real-time demod and with each other.
- Timing margin hardened (`DISPLACEMENT_BATCH_CYCLES`/`_RING_DEPTH`/
  `_MAX_CYCLES_PER_TICK`, see above) after board 2 exposed the original tuning as too
  thin in general, not board-specific.
- Zero calibration (180-degree reversal test) implemented, API-accessible AND now in the
  local SETTINGS menu (see above), mechanism bench-verified via the API. Needs re-running
  after the sensitivity bump, and still needs a real physical-flip test -- and the new
  local menu entry itself needs the user's own eyes/hands on the panel to confirm.
- LIVE screen redesigned to show displacement prominently (see above) -- visually
  confirmed working by the user on the physical panel.
- Deferred, not blocking: the high-rate per-cycle stream (nothing drains
  `svc_displacement_pop()` yet); a proper bench calibration of `gain`/`d0`/`zero_offset`
  against a certified reference (the ~7000x `d0` bump above is a coarse per-board sanity
  check, not that calibration); a real physical-flip zero-cal test; visual/
  hands-on confirmation of the new SETTINGS menu entry.
- **Open bug, not fixed by this session:** the LIVE screen's redraw cost drops a real
  ~500-cycle burst every `LIVE_DISPLACEMENT_REFRESH_MS` period, present even at the
  original 2000 ms/32-cycle settings -- see the section above and [[wp10-redraw-cost-bug]].
  `DISPLACEMENT_BATCH_CYCLES` (now 128) + a 4-sample moving average are in, giving a real
  ~12 dB SNR improvement independent of this bug, but the refresh rate itself is
  deliberately still at 2000 ms pending that bug's fix.

## Standard error vs averaging duration -- absolute vs differential (2026-09-27)

Post-hoc statistical analysis of already-archived data (`Testing/`), not a new bench
test -- answers two questions: ignoring drift, how long an averaging window gets an
absolute reading's standard error under 1µm (treating the current `d0`-calibrated
`delta_mm` output numerically as the "1µm/m" target unit, since `d0` was tuned toward a
±1mm/m range -- **caveat: this is NOT the same as a real inclination-angle calibration**,
which per this doc's intro is still pending pendulum/flexure characterization); and how
that changes for a *differential* measurement (one sensor left fixed as a reference,
the other moved to take readings -- a classic surface-plate comparator scheme).
Script: `PythonTestCode/stderr_vs_duration_analysis.py`, run against three independent
archived datasets (2-hour test's baseline hour, the pre-swap and post-swap 10-minute
streams).

**Method:** rather than trust the textbook SE=σ/√N formula, checked it empirically --
binned each trace into non-overlapping windows of N samples (N=1..5000, i.e. ~0.06s to
~280s at the ~18Hz stream rate) and measured the actual std of the bin means, comparing
against what 1/√N scaling from the per-sample noise (step std/√2, since consecutive
raw-batch samples showed the strong negative lag-1 autocorrelation -- roughly -0.4 to
-0.6 -- expected when independent per-batch noise dominates over real signal change
between adjacent ~56ms samples) would predict.

**Absolute (single-channel) SE does NOT keep improving with more averaging.** All three
datasets: predicted-vs-actual diverge fast -- by N=1000 (~56-58s) actual std is
20-70x worse than naive 1/√N prediction. The empirical std instead *plateaus*: ~2.1-2.4µm
(S1) / ~2.2-2.6µm (S2) in the 2-hour test's calm baseline hour; ~5.0-5.2µm in the
pre-swap run; a dramatic ~57-60µm in the post-swap run (a large real thermal/settling
transient after the physical handling dominated that window). **Conclusion: a single
absolute channel cannot reach sub-1µm/m standard error by averaging longer, under any of
the three real sessions measured** -- the noise has real power at long timescales
(consistent with everything else this session found about non-white, partly-shared
drift), not just short-term white noise that would average away.

**Differential (S1-S2) SE is dramatically better, in all three datasets, and crosses
under 1µm within roughly a second:**

| dataset | N=1 (~0.06s) | N=20 (~1.1-1.2s) | plateau (N≥500, ~28-280s) |
|---|---|---|---|
| 2-hour test baseline hour | 1.90µm | 0.52µm | ~0.44-0.49µm |
| pre-swap 10min | 6.61µm | 0.85µm | ~0.36-0.49µm |
| post-swap 10min | 1.84µm | 0.27µm | ~0.16-0.19µm |

Every dataset crosses under the 1µm target by roughly N=10-20 samples (~0.5-1.2s) and
settles to a plateau 4-10x better than either single channel's own floor. In the
post-swap run specifically, where a large shared thermal transient made the absolute
channels' std blow up to ~57-60µm, the differential reading was essentially unaffected
(~0.16-0.19µm) -- ~300x better -- because that transient was almost entirely common-mode
between S1 and S2 (matches the tight S1/S2 correlation already established in
[[wp10-charging-noise-and-drift]] and [[wp10-micro-jump-noise-characterization]]).

**Bottom line:** the target (<1µm/m-equivalent standard error) is not achievable from a
single absolute reading no matter how long you average, with the AFE as it stands today
-- but is comfortably (4-10x margin) and quickly (<2s) achievable with a differential
scheme, because most of the dominant noise/drift is common-mode between S1 and S2 and
cancels in the difference. Practical implication for a plate-flatness survey: leave one
sensor head at a fixed reference point and rove the other -- not because it's
theoretically elegant, but because it's the only one of the two schemes that has
actually been shown, on three separate real datasets, to hit the target.

## Differential reading exposed on the API + 64-cycle batch experiment (2026-09-27, fw 0.10.50)

Two follow-ups from the analysis above, both requested directly:

**1. Differential (S1-S2) now a first-class reading, not something a client derives.**
The original unit's own two supported configurations (one sensor connected = absolute,
both = differential) already anticipated differential being the dominant mode once both
are wired up -- this makes it so everywhere the two absolute channels are exposed:
- Measurements `0x0E` (`delta_diff_mm`, post-MA) and Topic groups `0x5/0x03`
  (`delta_diff_mm_raw` + `quality_diff_ok`, pre-MA) -- both exactly `delta1 - delta2`
  (a boxcar average is linear, so `MA(d1)-MA(d2) == MA(d1-d2)` exactly -- no separate
  moving-average state needed, just a subtraction getter).
- Triggered precision measurement (Commands `0x07`/Raw `0x04`): a THIRD accumulator,
  `delta_diff_mm`, alongside the existing per-sensor `delta1_mm`/`delta2_mm`. Critically
  this is NOT `delta1_mm - delta2_mm` -- it separately accumulates `delta1[i]-delta2[i]`
  for the same batch `i`, gated on BOTH sensors' quality flags being good on THAT batch
  ("exclude the differential reading if either input is bad," per the user's explicit
  ask). `count_diff` generally lags `count1`/`count2` slightly (it needs the AND of both,
  not either independently) and now gates run completion alongside them.
- LIVE screen: a new small "Diff ±X.XXXmm" line between S1/S2 and the temp/battery line
  (`App/app_display.c`, y=172; bottom line moved 180->204 to make room).

**Bench-verified over UART (fw 0.10.50):** Measurements `0x0E` and Topic `0x03`'s new
fields both agree with `delta1-delta2` computed client-side to the last bit (wire value
`0.11204147338867188` matched a client-side subtraction of the same two raw values
exactly). A triggered precision measurement completed in 2.04s with `count1=count2=
count_diff=64/64` (target reached on all three, zero timeout) -- confirms the joint
gating doesn't meaningfully lag the individual channels when quality is good, and that
the new ~40.7 Hz batch rate roughly halves the measurement's wall-clock time as expected.

**2. `DISPLACEMENT_BATCH_CYCLES` 128->64, `DISPLACEMENT_MA_SAMPLES` 4->8 (experiment,
user's own framing: "let's see if this makes things better").** Reasoning (see
config.h's own comments for the full derivation): division is effectively linear at this
system's operating point (x only ever sits ~1e-5..1e-6 from 0.5), so a big single
coherent batch and many smaller batches averaged afterward reach the same noise floor
for the same total integration time -- the real difference is that smaller batches let
the quality flag detect and exclude a transient at finer time resolution instead of
having it silently blended into one bigger division. Doubling `DISPLACEMENT_MA_SAMPLES`
alongside the halving keeps the smoothed value's total SNR exactly unchanged
(`sqrt(64)*sqrt(8) = sqrt(512) = sqrt(128)*sqrt(4)`) -- only the exclusion granularity
changes, not the live-display noise floor. `DISPLACEMENT_ZERO_CAL_SAMPLES` 32->64
alongside it, same "keep total raw-cycle depth/duration constant" reasoning as its own
earlier rescale.

**Bench-verified, not yet A/B'd against the old 128-cycle setting:** build is clean
(zero warnings, `-Wall -Wextra -Werror`), flashed and running fw 0.10.50. A 20s streamed
soak (Topic `0x03` at the 50ms subscription floor, so observed push rate is ~18 Hz
regardless of the faster underlying batch rate) showed: zero sequence gaps in the pushed
stream, quality-bad rates of 1.9%/5.8%/6.1% (S1/S2/diff) in the same ballpark as prior
sessions, and `input_drop_count` rising by 7626 over 20s (~14.6% of the ~52k cycles
produced in that window). That drop rate is real but **not attributable to this change
with confidence** -- the dominant driver of input drops was already identified
independently as the LIVE-screen redraw cost (~64% of drops) and BLE/LEDs (~33% more),
present at both 32 and 128 cycles/batch per [[wp10-redraw-cost-bug]]; doubling the
division rate (64 vs 128 cycles) could be a secondary contributor but no controlled A/B
(same screen state, same duration, old vs new firmware) has been run to isolate it.
**Open follow-up if a firm answer is wanted:** reflash the 128-cycle build and repeat
the identical 20s soak for a real before/after comparison.

## VBUS-disconnect grace period + LIVE-screen precision-measurement trigger (2026-09-27, fw 0.10.52)

Two independent user reports, addressed together.

**1. "With USB connected, the sensors at some point enter standby mode and there is no
way of waking them up other than power-off and restart."** Code review confirmed both
existing Standby-entry paths (`Services/svc_power.c`'s idle-timeout auto-poweroff and
`Services/svc_battery.c`'s critical-battery shutdown) already correctly gate on
`usb_connected` -- so this isn't a missing check. Most likely explanation: `VBUS_SENSE`
is a plain digital GPIO read with **zero debounce**, and this project's own known ~70mA
excess draw on the 5V rail (`current-consumption-investigation`) is plausibly enough to
sag VBUS momentarily on a marginal cable/port under a load transient (the AFE/display
drawing a spike) -- a single bad `svc_battery_update()` tick reading "unplugged" was
enough to un-suppress both shutdown paths even though USB never actually disconnected.

Fixed with a 10-second grace period on the **disconnect** direction only
(`Services/svc_battery.c`'s `VBUS_DISCONNECT_GRACE_MS`) -- a fresh connection still
registers immediately (charging starts promptly, unchanged), but `s_usb_connected`
(and therefore `g_system_state.usb_connected`, which both shutdown paths read) only
commits to "gone" after the raw GPIO has read low continuously for the full 10s. The
raw (undebounced) reading still gates whether `CHARGE_SENSE`/`STANDBY_SENSE` are trusted,
since those genuinely are undriven garbage without real VBUS regardless of how recently
it was present. Not bench-validated against a real VBUS glitch (would need the device's
own USB-C cable and a way to induce/observe a sag, not just the ST-Link's separate debug
power this session's own bench testing used) -- this is a reasoned fix for a plausible
mechanism, not a confirmed root-cause fix.

**Also clarified, not changed:** wake-from-Standby sources are exactly three
high-level-triggered pins -- `ENC_1SW` (WKUP1), `VBUS_SENSE` (WKUP4), `ENC_2SW` (WKUP5),
`HAL_App/hal_power.c`'s `hal_power_configure_wakeup_pins()`. Pressing either encoder
switch already wakes the device today (with a full reboot -- Standby always resets on
wake, all RAM lost, by STM32 design, not something firmware can avoid). **Encoder
*rotation* (A/B lines) cannot wake the device** -- those GPIOs simply aren't among the
STM32G0B1's WKUP-capable pins, a hardware pin-mapping constraint from the schematic, not
a firmware gap. Nothing implemented here; documented so a future hardware rev knows this
if "wake on any input including rotation" is wanted.

**2. "No way to trigger precision measurement from the physical device."** Implemented
exactly as suggested: right-knob (encoder 1) press on the LIVE screen now calls
`svc_displacement_precision_begin()` (`App/app_ui.c`) -- previously a no-op there ("RIGHT
encoder rotate/push do nothing" on LIVE/STATUS). A repeated press restarts a fresh run
(`precision_begin()`'s own documented behavior), so it can't get stuck. `DRV_ERR_NOT_READY`
(demod not running, or a zero-cal in progress) is silently ignored, same "no error
channel beyond the row's own state" reasoning `UI_SETTING_ZERO_CAL`'s handler already
uses.

Added on-screen feedback so triggering it isn't a black box: the LIVE screen's
temp/battery line temporarily shows precision-measurement progress (`Precision N/64...`)
while running, then the result (`Precision +0.1120mm`, 4-decimal like the Diff line, plus
`(partial)` if it timed out) for 15 seconds after completion
(`PRECISION_RESULT_DISPLAY_MS`, `App/app_display.c`), before reverting to the normal
temp/battery display. `svc_displacement`'s own `DISP_PRECISION_DONE` phase persists
indefinitely by design ("stays readable until the next begin()"), so the 15s window is
tracked locally in the display module, not sourced from the phase itself.

**Bench-verified (fw 0.10.52):** build clean (zero warnings), precision measurement via
the API unaffected (64/64/64 target reached, zero timeout, ~2.7s). The LIVE-screen
button binding and its on-screen feedback are code-reviewed but **not yet visually
confirmed on the physical panel** (needs the user's own eyes on the display + a real
knob press) -- same caveat this project routinely carries for display-only changes.

## Signal diagnostics screen + Wyler theoretical baseline (2026-09-27, fw 0.10.53)

Two changes for tonight's granite-plate calibration, both requested directly.

**1. DIAGNOSTICS screen** (`App/app_display.c`'s `draw_diagnostics_screen()`, a new
`UI_SCREEN_DIAGNOSTICS` in the LEFT-encoder screen cycle) shows amplitude (RMS +
peak-to-peak) and phase for all 4 raw ADC channels (B, A, S1, S2) -- the calibration tool
for the two Wyler sensors' own "zero" and "gain" trim pots. S1/S2 shown in microvolts
(their signal is small enough that millivolt resolution would hide exactly the digits a
pot adjustment needs), B/A in millivolts. Also exposed over the API as Topic groups
`0x5/0x04` (`docs/api-reference.md`) so the same numbers can be logged from a laptop at
the plate, not just read off the small on-device screen.

Math (`Services/svc_displacement.c`'s `svc_displacement_get_signal_diag()`): inverts
`math_phasor.c`'s per-cycle scale (one cycle's `i_sum/q_sum = 65536*A_peak_code*cos/sin(phi)`,
config.h's `DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG` comment has the same derivation) to
get back to a peak ADC code, then the ADS131M04's own LSB size
(`2.4V/PGA/ADS131M04_CODE_MAX`) to get millivolts AT THE ADC PIN -- i.e. after each
channel's own PGA (16 for S1/S2, 1 for A/B), which is the right reference point for
watching a physical trim pot's effect.

**2. Theoretical baseline, replacing the empirical D0_UM.** User's framing: "we
empirically added a gain of 7000x the original value. Now the Wyler handbook gives us
the precise (if theoretical) answer: 20uV RMS means 1um/m. Implement this as a baseline,
any digital calibration should just be a multiplier to this theoretical baseline."

**Derivation:** `delta_mm = 2*d0*(x_re-0.5)` is meant to read directly in mm/m (this
project's own established convention). The Wyler spec gives an INDEPENDENT tilt estimate
for the same batch, computed directly from S's own measured RMS amplitude (the
diagnostics screen above) with zero dependency on atten/gain/(A-B) at all:
`tilt_theoretical_mm_per_m = S_rms_mV / 20`. Solving for the `d0` that would make THIS
SAME batch's `x_re` reproduce that tilt exactly: `d0_theoretical = tilt_theoretical /
(2*(x_re-0.5))` -- exactly how the old `d0=700mm` was derived too (empirically, from one
known physical displacement), just with the Wyler handbook as the reference instead of a
paper-shim test.

**Computed from the one real dataset available** (`Testing/2026-09-25_gain_sizing_bulk_capture/data/gain16_check_pga16.csv`,
PGA=16, matching current hardware -- an uncontrolled bench tilt, not a certified
reference): `d0_theoretical` = 439mm (S1), 495mm (S2). **This is a genuinely useful
cross-check on its own**: two completely independent calibration methods (a physical
paper-shim test vs. a manufacturer's handbook spec) landing within 1.4-1.6x of each
other, not an order of magnitude apart, is a reassuring sign neither is wildly wrong.

**Implementation** (`system_state.h`, `Config/config.h`, `Services/svc_displacement.c`'s
`load_sensor_cal()`): `disp_s1/s2_d0_um` REPLACED by `disp_s1/s2_d0_theoretical_um`
(fixed, Wyler-derived anchor) and `disp_s1/s2_cal_mult_milli` ("any digital calibration on
top of that baseline" -- literally the user's own words). `effective_d0 = d0_theoretical *
cal_mult`. **`cal_mult` defaults were chosen to reproduce the OLD empirical d0=700mm
exactly** (1.595x for S1, 1.414x for S2) -- this refactor changes NOTHING behaviorally on
its own; it makes the existing calibration's distance from pure theory a single, always-
readable number instead of an opaque 700mm constant. EEPROM version bumped
(`EEPROM_DISPLACEMENT_SETTINGS_VERSION` 0x0004->0x0005, a struct layout change). API:
Calibrations `0x2/0x02` and `0x2/0x05` (old D0_UM) retired, replaced by `0x07`-`0x0A`
(theoretical + mult, per sensor) -- see `docs/api-reference.md`.

**Tonight's actual job:** the granite plate gives a REAL, known reference tilt --
compare the device's `delta1/2_mm` (or, better, `delta_diff_mm` -- see the differential
section above) against that known value, then adjust `cal_mult` (not `d0_theoretical`,
which stays fixed as the traceable anchor) until they agree. The DIAGNOSTICS screen's
`theoretical_tilt1/2_mm_per_m` gives a live, independent second opinion at the same time,
computed straight from the Wyler spec with no calibration constants involved at all.

**Bench-verified (fw 0.10.53):** build clean (zero warnings). New Calibrations fields
read back the correct new defaults (439000/1595/495000/1414) after the EEPROM version
bump; old 0x02/0x05 correctly return `UNKNOWN_RESOURCE`. Topic `0x5/0x04` decodes
correctly (B/A ~885-893mV RMS, S1/S2 ~34-39mV RMS at the bench's current, un-leveled
tilt -- consistent with earlier session findings); `theoretical_tilt1/2` computed live
alongside `delta1/2_mm` for direct comparison, exactly as intended. **Visually confirmed
on the physical panel by the user 2026-09-27** -- the DIAGNOSTICS screen renders
correctly.

## Per-position batch accumulation (fw 0.10.64, 2026-10-03)

**What changed.** The demodulation hot path (`on_sample()` in `Services/svc_displacement.c`,
run from the SysTick frame drain at the 20.8 kHz ADC rate) no longer multiplies each sample by its
Q14 DFT weight into 64-bit I/Q sums. It now does one plain `int32` add per channel per sample into
`s_pos_sum[channel][position]`, where `position = sample index mod 8`, over a whole batch of
`DISPLACEMENT_BATCH_CYCLES` (64) cycles. When the 64th cycle completes, the 32 sums (+ the seq of the
batch's last cycle, 130 B) are pushed into a small SPSC ring (`DISPLACEMENT_BATCH_RING_DEPTH` = 4
batches). `svc_displacement_update()` applies the weights once per batch with
`math_phasor_combine()` (`Math/math_phasor.c`):

    I = 16384*(s0 - s4) + 11585*(s1 - s3 - s5 + s7)
    Q = 16384*(s2 - s6) + 11585*(s1 + s3 - s5 - s7)

in int64, then hands the unchanged `BatchSums` to `process_one_batch()` (or the phasor log).

**Why.** The Cortex-M0+ has no 64-bit multiply: every `(int64_t)sample * weight` was a ~50 cycle
`__aeabi_lmul` library call, 8 per ADC sample (verified in the disassembly) = roughly 12.5 M cycles/s,
~20% of the 64 MHz core, the biggest term in the displacement CPU budget (see the "~18%-marginal"
discussion in `Config/config.h`). The new sample path is ~10 cycles per sample; the two remaining
64-bit multiplies per channel run once per batch.

**Result is bit-identical.** The weighting is linear integer arithmetic, so the combined I/Q equal
the old per-sample accumulation exactly (checked against the reference `math_phasor_accumulate()` on
2005 random and full-scale cases in a Python model; the C host test `tests/test_math_phasor.c` does
the same but could not be run on the dev box, which has no host compiler -- run `make` in `tests/`
where one is available).

**Why int32 is enough.** A position sum adds 64 signed 24-bit codes: |sum| <= 64*2^23 = 2^29 (fits
int32 for up to 256 cycles per batch; `_Static_assert` on `DISPLACEMENT_BATCH_CYCLES`). Deterministic,
so no overflow handling is needed; the 4-term groups in the combine can reach 2^31 and are formed in int64.

**Behavioural differences:**
* The input ring now holds batches, not cycles: 4 batches = ~98 ms of slack (was 128 cycles = ~49 ms)
  for 520 B of RAM instead of 8.4 KB (RAM 90.8% -> 84.9%).
* No partial batches. If the ring is full when a batch completes, the whole batch is dropped and
  `input_drop_count` is incremented by 64 (still counted in cycles); `seq` still advances per cycle, so a
  drop appears as a seq jump of a multiple of 64 (the phasor-log `gap_cycles` column).
* `DISPLACEMENT_MAX_CYCLES_PER_TICK` (64 cycles/tick) became `DISPLACEMENT_MAX_BATCHES_PER_TICK`
  (2 batches/tick), the same livelock guard expressed in batches.
* `math_phasor_accumulate()` is kept only as the test oracle.

**Not yet verified on hardware** (the board was not attached when this was written): flash fw 0.10.64
and compare `Raw data 0x7/0x02` (clip/drop counters, max update gap) and the readings against 0.10.62/63.

## Continuous phasor batch stream (2026-10-04, fw 0.10.65)

The one-shot phasor log (512 batches, then a ~2.6 s transfer pause) cannot give a gapless
40.7 Hz series, and the interval-based Topic streams only snapshot the latest value at
<= 20 Hz (aliasing the ~20 Hz pendulum). New API Topic 0x05 / resource 0x05 pushes **every**
completed 64-cycle batch: `svc_displacement_update()` routes batches into a 64-entry FIFO
(`DISPLACEMENT_PHASOR_STREAM_DEPTH`, 34 B each, `store_phasor_stream_entry()`, drops the
newest on overflow and counts it) instead of the demod/log, and `svc_api.c`'s
`phasor_stream_pump()` hands one frame per batch to the transport, only while its TX ring has
headroom (`ready_fn`); an entry leaves the FIFO only once sent. Subscribe = start the ADC
(refused with BUSY_EXCLUSIVE while the demod or a bulk capture runs), unsubscribe or a
disconnect = stop. A lost batch shows as a cycle-`seq` jump (multiple of 64), a lost frame as
an `issue_seq` jump. Frame = `[status][issue_seq][page=0]` + the 34-byte log entry, ~1.75 kB/s.
Bench (UART, 1 min): 2443 batches, 40.71/s, zero gaps/losses. Host logger:
`Testing/2026-10-04_contiguous_phasor_capture/phasor_stream.py`.

## Display stream, ~4 Hz (2026-10-05)

The LIVE screen no longer shows the 40.7 Hz batch values. `svc_displacement` condenses the raw
per-batch deltas into a display stream: triangular window over `2N-1` = 19 batches (two cascaded
10-batch boxcars) and decimation by `N` = 10, i.e. 4.07 Hz with a ~0.47 s window
(`DISPLACEMENT_DISPLAY_DECIMATION` in `config.h`). The filter has exact nulls at multiples of
4.07 Hz, including 20.35 Hz, the batch Nyquist frequency where the ~20 Hz pendulum lands, so the
resonance is suppressed rather than aliased (findings of `Testing/2026-09-30_bulk_adc_30s_interval`).
API: `svc_displacement_get_display_delta1/2/diff_mm()`, `_display_seq()` (the redraw trigger),
`_display_valid()` (false until the first window is full, ~0.5 s after a start). The API
Measurements resources and the 8-batch boxcar are unchanged. Build-verified only.

## Phase calibration, approach 2 (2026-10-06, fw 0.10.66)

Implements `docs/signal_processing.tex` Sec. 9.1. Per sensor the phasor ratio `u = S/(|k| D)`
(D = A - B, measured every batch) is rotated by `-delta` before the in-phase projection:
`x_re = u_re cos(d) + u_im sin(d)`, `x_im = -u_re sin(d) + u_im cos(d)`. `delta` is stored as
`disp_s1_phase_cdeg` / `disp_s2_phase_cdeg` (int16, centidegrees, negative = sensor early), API
Calibrations 0x0D / 0x0E; defaults -6.00 / -9.15 deg from the principal axis of S/D over the 19 h
phasor stream. EEPROM displacement page version 0x0009: the page is reseeded to defaults on the
first boot (gains, sensitivities, zero offsets and invert flags must be set again; zero-cal and
sensitivity are due for a redo anyway). The diagnostic phases (DIAGNOSTICS screen, API signal
diagnostics) are now relative to D at 90 deg, 0..360. Build-verified only.

## |k| as one empirical number (2026-10-06, fw 0.10.67)

The separate shared attenuator constant (`disp_atten_milli`, API Calibrations 0x00) is removed.
`disp_s1/s2_gain_milli` is now |k| of each sensor: one empirical scale between the sensor path and
the A/B path, everything folded in (on-board attenuation and filtering, ADC PGA, the instrument's
gain). Default 480.000 = the old attenuator 3.000 x gain 160.000, so readings are unchanged. EEPROM
displacement page version 0x000A (layout change): the page is reseeded to defaults on the first boot.
Build-verified only.

## |k| stored for PGA = 1 (2026-10-06, fw 0.10.68)

`disp_s1/s2_gain_milli` (|k|) is now stated and stored for PGA = 1; `svc_displacement.c` multiplies it by
the sensor channel's PGA before dividing S (`load_sensor_cal()`). The per-channel PGA gains are defined
once, in `drv_ads131m04.h` (`ADS131M04_PGA_S1/S2/A/B`); the ADC's GAIN1 register value and the diagnostic
voltage scaling are derived from them. Default 30.000 (= the old 480 at PGA 16), so readings are unchanged.
EEPROM displacement page version 0x000B (same layout, new meaning): reseeded on first boot. Build-verified only.

## Calibration redesign: k and zero (2026-10-06, fw 0.10.69)

Per sensor the calibration is now `tilt [mm/m] = (r - zero) / k`, with `r = Re[S/(PGA*D)*e^{-j delta}]`.
`k` (`disp_s1/s2_k_micro`, int32, x1e-6 per mm/m, stated for PGA 1, default 21300 = 0.0213 derived from the
nominal 20 uV/(um/m) and the network gains, see `docs/signal_processing.tex` Sec. 13.3) and `zero`
(`disp_s1/s2_zero_ppm`, ppm of `r`, independent of `k`). Removed: the attenuator, `d0_theoretical`,
`sensitivity` and the old `zero_offset_um`. API Calibrations 0x01/0x04 = k, 0x03/0x06 = zero (ppm),
0x00 and 0x07..0x0A retired. Zero-cal converts its mm result to ppm via k. EEPROM displacement page
version 0x000C: reseeded on first boot. Older Testing scripts that SET the retired resources need updating.
Build-verified only.

## Per-sensor zero calibration (2026-10-06, fw 0.10.70)

The 180-degree flip calibration can run for S1 only, S2 only or both. `svc_displacement_zero_cal_step1_begin(mask)`
(bit 0 = S1, bit 1 = S2); step 2 continues the same sensors; only the selected sensors' `disp_sN_zero_ppm` are
written. API: Commands 0x06 step 1 accepts an optional 2nd byte (sensor mask, default 3), Raw data 0x03 appends
`sensor_mask`. Instrument: SETTINGS rows "Zero cal both / S1 / S2". Build-verified only.
