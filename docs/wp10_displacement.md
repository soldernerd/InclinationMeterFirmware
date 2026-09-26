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

## Current status (fw 0.10.43)

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
