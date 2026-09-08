# ADC acquisition redesign — `ADC_Optimization` branch

**Goal:** cut the ADS131M04 acquisition CPU cost (~15–20% when running, almost
all of it ISR-resident) and make *any* lost or duplicated conversion a
detectable ERROR on the debug-log stream.

Baseline (master, `wp8-debugged`): TIM7 fires a lean ISR at **41 666.67 Hz**
(2× fDATA). Each fire runs `on_trigger()` — a 3-state DRDY poll that kicks a
raw SPI1 DMA frame read, and on completion does `sign_extend24 ×4` +
`svc_signal_analysis` MAC, all in that priority-0 ISR. Roughly:

| ISR | rate | ~CPU |
|---|---|---|
| TIM7 `on_trigger` (poll + DMA setup/teardown + sign-extend) | 41.7 kHz | ~10–15% |
| `on_sample` DFT MAC (nested) | 20.8 kHz | ~3–4% |
| SysTick | 1 kHz | ~0.1% |

About half the 41.7 kHz fires find DRDY not ready and do nothing useful yet
still pay the full Cortex-M0+ exception entry/exit.

DRDY (PA1) cannot be an EXTI — EXTI1 is taken by PB1/ENC_2A. **Next hardware
rev: move DRDY to a pin with a free EXTI line.** Until then it is polled.

---

## Target architecture

### Buffer + drain

- SPI1 RX DMA writes each frame **directly into the next ring slot**
  (`ADC_FRAME_RING_FRAMES` = 64, 64 × 18 B ≈ 1.2 KB) — zero-copy. The TIM7
  trigger ISR only advances `s_ring_head` and re-arms the DMA at the
  following slot; no `memcpy` in the ISR (a byte-copy loop there at the
  Debug `-O0` cost near-wedged the scheduler — bench, fw 0.9.15).
- Consumer runs from **SysTick (1 kHz)**, not PendSV. A PendSV soft-IRQ
  re-pends itself and, being higher exception priority than SysTick, can
  starve thread mode outright once it can't drain faster than it's pended
  (bench, fw 0.9.13–0.9.15). A periodic tick can't: it does ~21 frames of
  work per ms and returns. Slack before the ring wraps is ~2 ms.
- The per-sample sign-extend + single-bin DFT MAC is heavy at `-O0`;
  `Services/svc_signal_analysis.c` and `Drivers_App/drv_ads131m04.c` are
  pinned to `-O2` in every build config (`CMakeLists.txt`) so the SysTick
  drain stays ~40 µs/ms instead of ~900 µs/ms.

Consumers (`svc_signal_analysis`'s DFT MAC and the bulk-capture store) are
unchanged — they still receive one `on_sample(ch0..3)` per conversion, just
from the SysTick drain instead of the TIM7 ISR.

### Pacing / CS

TIM7 and fDATA are **frequency-locked** — both derive from the 64 MHz
SYSCLK (fDATA = SYSCLK/12/OSR via the TIM2 MCLK PWM; TIM7 period 1535 →
exactly 2× fDATA). The original "drifting phase" was a 1× attempt sitting on
the DRDY edge; at 2× every conversion is read within one period.

- **Phase 1** keeps the proven per-frame mechanism: TIM7 ISR still polls
  DRDY, toggles CS, kicks the 18-byte DMA. It only stops doing the heavy
  work inline — it pushes the raw frame into the ring and pends PendSV.
- **Phase 2** (only if Phase 1's residual ISR cost still matters): hold CS
  low continuously, RX DMA free-running circular, TIM7 ISR reduced to a
  bare 18-byte TX-DMA kick (~15 instr). Needs bench validation that
  continuous-CS streaming is reliable on this board and that MODE.TIMEOUT
  interaction with any inter-frame SCLK gap is benign. A fully CPU-free
  variant (DMAMUX TIM7_UP → TX DMA) is possible but risks bursty in-frame
  SCLK; not pursued unless the kick ISR proves too expensive.

---

## Integrity — every lost/duplicated conversion is an ERROR

Layered, each giving certainty, all feeding dedicated counters:

| check | where | catches | param-free? |
|---|---|---|---|
| **Ring overflow** — DMA write index laps the drain read index | drain entry | drain starved / too slow | yes |
| **Conversion count** — every frame's word-0 DRDY bits say fresh vs stale; fresh-frame count vs. expected (kicks/2 ± 1), cumulative deviation | drain | a dropped or duplicated conversion, to ±1 | yes |
| **Frame CRC** — ADS output CRC word (frame bytes 15..16) vs CRC-16/CCITT over bytes 0..14 (reuse `math_crc`) | drain, per frame | corruption **and** byte-misalignment (CS/SCLK integrity — the continuous-CS risk) | needs bench-confirmed byte range/justification |
| **Timer cross-check** — `TIM2->CNT` (same clock domain as fDATA) elapsed / OSR vs. fresh count over a longer window | periodic, task ctx | gross timing failure as a backstop | yes |

On the **first** integrity error of an acquisition run:
`svc_log(API2_LOG_ERROR, ...)` with cause + counter (goes out on debug
stream, API v2 cat 0x6), then **latch the pipeline STOPPED** — a
measurement stream that has lost sample integrity must not silently
continue; require an explicit API restart. Non-latching is opt-in only.

Counters (separate, cause matters): `ring_overflow`, `conv_slip`,
`crc_err`, `framing_err`. Exposed in the `Raw data 0x00` diagnostic GET
alongside the existing samples/drops/elapsed.

Error-log throttling is moot with latch-stop; if a non-latching mode is
ever added, coalesce to ≤1/s with a running count or the log floods the
TX ring.

---

## Phasing (each phase is independently mergeable)

**Phase 1 — relocate the consumer. DONE (fw 0.9.17, bench-verified).**
Zero-copy DMA-into-ring; TIM7 ISR advances head + re-arms; SysTick drains
(sign-extend + `on_sample` + ring-overflow / clamp counters), and records
word-0 / CRC bytes for API read-back (`Raw data 0x00`) so Phase 2's
integrity gate is built on confirmed facts. Hot path forced to `-O2`.

Bench (fw 0.9.17): 60 s continuous — rate 20731–20853 Hz (nominal 20833),
`backlog` steady 6–8 frames, `ring_overflow` 0, `drain_clamped` 0. Bulk
capture 6144 samples, 0 gaps; tone 2604.1 Hz, 77–78 dB SNR on ch1/ch2;
ch0/ch3 at the noise floor. Observed: `word0` (STATUS response word) is a
constant `0x010F` every frame — its DRDY bits do **not** toggle
fresh/stale, so Phase 2's conversion-count check can't lean on them; the
CRC word does vary per frame (candidate for the CRC check).

### API-stall root cause — RESOLVED (fw 0.9.38)

The API "stall" seen while acquisition runs was `task_display`: the
cooperative scheduler is single-threaded, and one `task_display` call
does a full 400x240 u8g2 render + frame-buffer parse. At the Debug `-O0`
default that render is ~120 ms; the acquisition ISR load (`on_trigger` +
the SysTick drain, themselves partly `-O0`) inflated it **~7x to ~875 ms**,
and it fires ~1/s (a 1 Hz value on screen changes) — so `task_api` was
blocked ~875 ms every second. `usb_irq` counting during the stall showed
only the normal ~1000/s SOF rate: not a USB storm (the `usb: host
disconnected` that sometimes followed was a *symptom* — 875 ms of missed
USB servicing).

Fix: `-O2` on the acquisition hot path (`hal_spi.c`, `hal_gpio.c`,
`math_crc.c` added to `drv_ads131m04.c` / `svc_signal_analysis.c`) **and**
the display render path (the `u8g2` sources, `app_display.c`,
`u8g2_hal_callback.c`, `drv_sharp_lcd.c`) — all in `CMakeLists.txt`, every
config. Results: `on_trigger` 10.8 -> 5.6 us; worst SysTick drain
998 -> 415 us (0 drains >= 1 ms); `task_display` 875 -> 108 ms and it now
fires only on a real content change, not every second. **API round-trip
latency during signal analysis: median 58 ms (= idle), p99 122 ms,
max 123 ms** over 615 round-trips. Flash *shrank* 8 KB (u8g2 at `-O2`).

Residual: an occasional ~950 ms latency spike appears at **idle too**
(in fact worse than during acquisition), ~0.1/s — host-side (the poisoned
dev-PC USB stack, see memory `wp4-usb-working`), not firmware.

**Phase 2 — integrity gate. DONE (fw 0.9.23, bench-verified).**

| check | mechanism | bench result | action |
|---|---|---|---|
| **framing** | every frame's word 0 must equal `ADS131M04_STATUS_WORD` (0x010F) | constant over long runs | **latch + ERROR + stop** |
| **CRC** | `math_crc16` over frame bytes 0..14 vs the ADS CRC word (bytes 15..16) | `calc == rx` every frame, confirmed | **latch + ERROR + stop** |
| **ring overflow** | SPI DMA a whole ring ahead of the SysTick drain | never seen | **latch + ERROR + stop** |
| **conversion count** | `frames_produced` vs the SysTick ms clock. fDATA = SYSCLK / `ADS131M04_FDATA_TIMER_TICKS` and SysTick shares the SYSCLK root, so `expected = elapsed_ms x ADC_FDATA_KHZ_NUM / ADC_FDATA_KHZ_DEN` exactly. Reference point taken once after `ADC_SLIP_SETTLE_MS` so pipeline startup is excluded. `deficit = expected - actual`. | **stays within +/-3 frames over 4 minutes** (~0.8 ppm) — no loss | `deficit` past `ADC_FRAME_DEFICIT_LIMIT` (8) for `ADC_FRAME_DEFICIT_HOLD_MS` (200) -> **latch + ERROR + stop** |

`Services/svc_signal_analysis.c`'s `svc_signal_analysis_check_integrity()`
(pumped from `task_signal_analysis`) does the reporting/stop. Counters,
`run_ms`, `frame_deficit` + range, and CRC calc/rx in the `Raw data 0x00`
diag.

### Resolved: the "~40 ppm sample loss" was a measurement bug

Earlier (fw 0.9.18–0.9.24) the conversion-count check used
`tim7_fires / OVERSAMPLE` as the reference — `tim7_fires` being a software
counter bumped once per TIM7 ISR. Bench measurement against the SysTick
clock (fw 0.9.24): the **frame rate is +0.9 ppm** (crystal-exact), while
the **fire rate is −21 ppm** — the lean TIM7 ISR occasionally gets pushed
past a full 24 µs period and only one `UIF` latch is seen for two elapsed
periods, so `tim7_fires` undercounts. The DRDY-level state machine catches
up on the next fire (DRDY still asserted), so no conversion is skipped.
The reference is now the SysTick clock; `tim7_fires` is kept only as an
informational ISR-miss indicator. No sample is being lost.

### 1x oversampling — tried, does not work reliably

Re-tried on the clean Phase 1/2 read path with the SysTick-referenced
integrity check watching (fw 0.9.27 / 0.9.29). Frequency-lock does not
help: the phase between TIM7 and the ADS conversion is random per
`start()`, and an unfavourable one puts every poll on the DRDY edge with
zero read margin. Observed across runs — mostly a slow ~0.5 lost-frame/s
drift; occasionally catastrophic (~120/s, `ADS_FAULT_SLIP` within ~15 s).
2x holds `frame_deficit` at 0 indefinitely. **Kept at 2x.** The 2x is not
waste — it is the timing margin the polled read needs. Cutting the ISR
cost further requires the no-CPU **timer -> DMA** read (below); the
`ADC_TRIGGER_OVERSAMPLE` plumbing stays in place for that.

**Merge to master when the whole thing is bench-clean.**

---

## Files

- `Drivers_App/drv_ads131m04.c/.h` — frame ring, PendSV drain, integrity
  counters, `drv_ads131m04_get_integrity()`
- `Core/Src/stm32g0xx_it.c` — `PendSV_Handler` body; DMA1_Ch2/3 IRQ if
  Phase 2 uses circular RX
- `HAL_App/hal_spi.c` — Phase 2 circular RX helpers
- `Config/config.h` — `ADC_FRAME_RING_FRAMES`, drain batch, integrity limits
- `Services/svc_api.c` — extend `Raw data 0x00`
- `PythonTestCode/adc_diag.py` — surface the new counters
