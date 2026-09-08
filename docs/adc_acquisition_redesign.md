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

- SPI1 RX DMA writes frames into a **64-frame circular ring**
  (`ADC_FRAME_RING_FRAMES`, 64 × 18 B ≈ 1.2 KB).
- Consumer runs from **PendSV at the lowest NVIC priority (3)** — preempted
  by SysTick and every real timer/DMA/comms IRQ, preempts nothing that
  matters. Its only hard deadline is "drain before the ring wraps": with a
  half-ring drain that is ~32 frames ≈ 1.5 ms of slack for a ≈ 60 µs job.
- Drain granularity: **half the ring (32 frames)** per wake — pended at the
  half and full points. Bigger batches amortise the fixed exception cost;
  32 is far below any latency any consumer needs (the DFT batch is 512
  samples ≈ 24 ms before it produces a number).

Consumers (`svc_signal_analysis`'s DFT MAC and the bulk-capture store) are
unchanged — they still receive one `on_sample(ch0..3)` per conversion, just
from the PendSV drain instead of the TIM7 ISR.

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

**Phase 1 — relocate the consumer (this iteration).**
TIM7 ISR unchanged except it pushes the raw frame (+ a `TIM2->CNT`
stamp) into the ring and pends PendSV instead of doing sign-extend + MAC
inline. PendSV drain (prio 3) does sign-extend + `on_sample` + ring-
overflow detection, and *records* word-0 / CRC behaviour for API
read-back so Phase 2's integrity gate is built on confirmed facts.
Bench gate: same ~20 833 Hz, 0 drops, tone at 2604 Hz, scheduler
responsive (clean status LED), integrity counters zero.

**Phase 2 — integrity gate + optional continuous-CS.**
Turn the recorded observations into the live checks above (dedup
mechanism, CRC with confirmed range, latch-stop). Optionally switch to
continuous CS + circular RX DMA and reduce the TIM7 ISR to a bare kick.

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
