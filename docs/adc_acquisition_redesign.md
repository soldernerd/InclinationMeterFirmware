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
CRC word does vary per frame (candidate for the CRC check). API
round-trip latency while running has an occasional ~245 ms bump (~1/s);
this is **pre-existing** — master shows the same at ~450 ms — and PC
sampling during it shows the cooperative loop spinning normally, no
single hog, so it reads as scheduler-jitter / host-serial interaction
under the added ISR load rather than a hard stall. Left for a separate
look; not a regression from this work.

**Phase 2 — integrity gate. DONE (fw 0.9.23, bench-verified).**

| check | mechanism | bench result | action |
|---|---|---|---|
| **framing** | every frame's word 0 must equal `ADS131M04_STATUS_WORD` (0x010F) | constant over long runs | **latch + ERROR + stop** |
| **CRC** | `math_crc16` over frame bytes 0..14 vs the ADS CRC word (bytes 15..16) | `calc == rx` every frame, confirmed | **latch + ERROR + stop** |
| **ring overflow** | SPI DMA a whole ring ahead of the SysTick drain | never seen | **latch + ERROR + stop** |
| **conversion slip** | `frames_read - tim7_fires/OVERSAMPLE`; TIM7 is exactly `OVERSAMPLE x` fDATA (locked SYSCLK divisors), so this is the exact lost/duplicated-conversion count. Band learned over `ADC_SLIP_SETTLE_FRAMES`, then any excursion counted. | **drifts ~0.4/sample-per-second even at the healthy 2x rate** — a real, slow read-path loss (see below) | **count + one WARN**, do NOT stop |

`Services/svc_signal_analysis.c`'s `svc_signal_analysis_check_integrity()`
(pumped from `task_signal_analysis`) does the reporting/stop. Counters +
learned band + CRC calc/rx in the `Raw data 0x00` diag.

### Open: the ~0.4/s conversion slip at 2x oversample

Bench (fw 0.9.19–0.9.23): with acquisition running, `frames_read` falls
behind `conversions_completed` by ~1 every ~2–3 s — monotonic, ~25–40 ppm.
So roughly one conversion in ~50 000 is missed in the read path. TIM7 and
fDATA are frequency-locked (both exact 64 MHz divisors) so this is not
clock drift; raising the TIM7 NVIC priority 2→0 barely moved it. It is
almost certainly **pre-existing** (master's in-ISR read had no way to
measure it) and, at 25–40 ppm, invisible to anything short of a
minutes-long capture — the single-bin DFT's running accumulator and the
0.3 s bulk capture both tolerate it. Left as a known item; the real fix
is the no-CPU-ISR **timer → DMA** acquisition path.

### Not done: 1x oversampling

Blocked on the slip above — 2x already loses samples; 1x has less margin
and would lose more. `ADC_TRIGGER_OVERSAMPLE` + `ADS131M04_TRIGGER_TIMER_PERIOD`
are now wired through so the change is a one-liner once the read path is
fixed (`hal_tim_adc_trigger_start()` sets the ARR from the macro — the
CubeMX literal in `tim.c` no longer matters).

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
