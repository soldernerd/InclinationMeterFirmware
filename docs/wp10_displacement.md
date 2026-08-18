# WP10 — Differential-Capacitor Phasor Demodulation and Displacement — Status

**Goal:** compute per-cycle displacement (δ) for two differential-capacitor sensor heads
(S1, S2) sharing a common antiphase excitation pair (A, B), from the same 4-channel
ADS131M04 ADC stream WP8 already brought up. Stops at δ — converting to inclination angle
needs empirical pendulum/flexure calibration and is later work. Branches from
`wp9@869d33c` (the tip after WP9's docs commit).

---

## Requirements (as specified)

- Physical model: `x = C_A/(C_A+C_B)`, `δ = 2*d0*(x - 0.5)` for a parallel-plate differential
  capacitor at neutral gap `d0`.
- Channel mapping: Input 1=B, Input 2=A, Input 3=S1, Input 4=S2. A/B attenuated by `atten`
  (shared, nominal 3) before the ADC; S1/S2 each amplified by `G` (per-sensor, nominal 10)
  before the ADC. Only `k = atten*G` matters for computing x, but G and atten are stored and
  calibrated separately.
- Per-cycle (8 samples/cycle) I/Q phasor extraction via a direct weighted sum (the trivial
  DFT-bin coefficients), not a general DFT/FFT, for A/B/S1/S2 every cycle.
- `x1/x2` via complex division; the imaginary-part residual is a diagnostic (should sit near
  0) and must be retained, not discarded.
- The per-cycle δ1/δ2 stream must be retained (not collapsed to a single filtered value) for
  later higher-frequency analysis (e.g. pendulum swinging).
- Calibration: one EEPROM page per sensor (G, d0, zero_offset, CRC), plus one shared page for
  atten — following whatever convention `Services/svc_storage.c` already uses.

## Pre-implementation research (as requested)

- **ADC driver interface**: `Drivers_App/drv_ads131m04.c` delivers samples via
  `Ads131m04SampleCb(int32_t ch0, ch1, ch2, ch3)`, fired once per raw sample at
  20833.33 Hz, confirmed exactly 8 samples/cycle (`Config/config.h`'s
  `ADS131M04_OSR_FIELD` derivation — this WP's 8-samples/cycle assumption already matched
  what WP8 built, no surprises there). Only **one** callback can be registered at a time
  (`drv_ads131m04_set_on_sample()` takes a single global function pointer) — WP8's
  `svc_signal_analysis.c` already occupied that slot with a generic 4-channel
  amplitude/phase diagnostic. Nothing in the repo documented which ADS131M04 channel is
  physically A/B/S1/S2, so this was confirmed with the user directly rather than guessed
  (see "Channel mapping" below).
- **Calibration storage convention**: `Services/svc_storage.c` has two patterns, not one —
  (1) `DeviceSettings`' `SettingsSection`/`offsetof` mechanism (many small per-subsystem
  field groups packed into one struct, each its own page) and (2) `CalibrationData`'s
  standalone-struct-with-dedicated-page pattern (`svc_storage_save_calibration()`/
  `load_calibration()`, not threaded through `DeviceSettings` at all). Initially used
  pattern (1) for S1/S2/atten — this was wrong (see "EEPROM payload budget" below) — the
  correct fit is pattern (2), extended to a reusable generic form (see "Calibration
  storage" below).
- **CRC helper**: `Math/math_crc.c`'s `math_crc16()` (CRC16-CCITT) — reused as-is, per the
  task spec.

## Two decisions confirmed with the user before implementing

1. **Channel mapping** — Input N = ADS131M04 CHN-1: `ch0=B`, `ch1=A`, `ch2=S1`, `ch3=S2`.
   No existing hardware doc states this; recorded in `Config/config.h`'s and
   `Services/svc_displacement.c`'s comments now that it's confirmed.
2. **`svc_signal_analysis.c` (WP8)** — retired outright rather than extending
   `drv_ads131m04.c` to support multiple callbacks. Matches its own header comment
   ("first cut... additional math may follow") and avoids two consumers fighting over one
   callback slot for a generic diagnostic this WP's math fully supersedes.

## Register/math-level design

- **x derivation** (ADC-domain, not "true" physical volts): the ADC sees `S_adc = G*S_true`
  for the S channel and `A_adc = A_true/atten`, `B_adc = B_true/atten` for A/B. Substituting
  into `S_true = x*A_true + (1-x)*B_true` and solving for x in terms of the *measured*
  quantities: `x = (S_adc/(G*atten) - B_adc) / (A_adc - B_adc) = (S_adc/k - B_adc)/(A_adc-B_adc)`,
  `k = atten*G` — matches the task spec's formula exactly, derived rather than assumed.
- **Digital scale factor cancels**: every channel (A, B, S1, S2) is demodulated by the
  *identical* per-sample transform (`Math/math_phasor.c`'s Q14 table, same 8 samples/cycle),
  so the raw `int64` I/Q sums all carry the same arbitrary digital scale factor. Since x is a
  ratio, that factor cancels completely — the raw sums can be fed into the complex-division
  formula directly (just cast to `float`), no separate "normalize to physical amplitude" step
  needed. Simpler than WP8's amplitude/phase math, which did need that normalization.
- **Complex division** (`Math/math_phasor.c`'s `math_complex_div()`): standard
  `(re1+j·im1)/(re2+j·im2) = [(re1·re2+im1·im2) + j(im1·re2-re1·im2)] / (re2²+im2²)`. Returns
  `false` on an exactly-zero denominator (A and B phasors identical — degenerate excitation,
  shouldn't occur in practice) instead of dividing by zero; `svc_displacement.c` counts this
  and skips the cycle rather than crashing or producing garbage.
- **S/k is a real-scalar division**, not a second complex division — `k = atten*G` has no
  imaginary part, so `S/k` just divides both the real and imaginary parts of the S phasor by
  the same real scalar before the complex subtraction `S/k - B`.

## Architecture — ISR/task split, same shape as WP8 but per-cycle not per-batch

- **ISR** (`on_sample`, registered via `drv_ads131m04_set_on_sample`): per-sample
  `math_phasor_accumulate()` (cheap `int64` multiply-accumulate) for all 4 channels. At each
  8-sample cycle boundary, snapshots the raw I/Q sums into a lock-free SPSC ring buffer
  (`DISPLACEMENT_RING_DEPTH` = 64 entries, ~24.6 ms headroom) and resets. Drop-newest-on-full
  with a saturating counter (CLAUDE.md 8.3/7.6) — WP8's batching hid this same tradeoff behind
  a single-slot double-buffer; running every cycle instead of every 64 needed a real queue.
- **Task** (`svc_displacement_update()`): drains *everything* queued since the last call —
  unlike every other scheduler task in this codebase (period ≥ 1 ms), this one must run every
  single scheduler tick, not at `task_sensors_ms`'s slower rate, or the ~2.6 kHz production
  rate would overrun the 64-entry buffer within a fraction of one 100 ms period. Per cycle:
  normalizes to float, computes `k1`/`k2`, complex-divides for `x1`/`x2`, then `δ1`/`δ2` and
  the residual (`Im(x)`), and pushes the result into a second output ring buffer (same depth)
  — this is the "raw per-cycle stream" the task requires, not just a filtered/averaged value.
  Also updates `g_system_state.disp1_delta_mm`/etc. with the latest cycle's snapshot, for a
  quick "current value" read (future UI/API use) separate from the full stream.
- `svc_displacement_pop()` drains the output ring buffer one entry at a time — the API a
  future WP would use to actually analyze the retained stream (e.g. for pendulum-swing
  detection); this WP only produces and retains it, doesn't consume it further.

## Calibration storage

- `system_state.h`: two new standalone structs, `DisplacementSensorCal` (`gain`, `d0_mm`,
  `zero_offset_mm` — two instances, `g_disp_s1_cal`/`g_disp_s2_cal`) and
  `DisplacementSharedCal` (`atten` — one instance, `g_disp_shared_cal`). **Not**
  `DeviceSettings` fields — see "EEPROM payload budget" below for why.
- `Services/svc_storage.c`/`.h`: generalized `svc_storage_save_calibration()`/
  `load_calibration()`'s single-blob mechanism into reusable
  `svc_storage_save_blob(addr, version, data, len)`/`load_blob(...)` functions —
  `CalibrationData`'s save/load are now thin wrappers around these, and the three new WP10
  structs use them directly rather than needing three more hand-copies of the same
  header+CRC+version bookkeeping.
- Three new pages: `EEPROM_DISP_S1_SETTINGS_ADDR` (`0x0700`), `..._S2_...` (`0x0800`),
  `..._SHARED_...` (`0x0900`) — extends `Config/config.h`'s existing per-subsystem page
  table, and the existing full-pairwise (not just adjacent-chain) address-distinctness
  `_Static_assert` in `svc_storage.c` now covers all `C(10,2)=45` pairs.
- Reseed-on-failure at boot (`svc_storage_init()`), same as `DeviceSettings`' pattern — unlike
  `CalibrationData` (deliberately left zeroed/invalid if missing, since it has no sane
  default), these three structs DO have sane nominal defaults (`gain=10`, `d0=0.1 mm`,
  `atten=3`), so a missing/corrupt page is reseeded with defaults rather than left as-is.
  Divisor-zero guards on `gain`/`atten` after load, mirroring
  `svc_storage_validate_settings()`'s "belt and suspenders" reasoning (no host-writable path
  exists yet for these structs, so this currently only defends against a corrupt-but-CRC-valid
  page — a future `SET_DISPLACEMENT_CAL`-style API handler would need the same check on an
  untrusted payload).

### EEPROM payload budget — the one real course-correction during implementation

First attempt put the 7 new fields directly into `DeviceSettings`, following the
`SettingsSection` pattern used by every other subsystem so far. This **broke the build**:
`Services/svc_api.c` has `_Static_assert(sizeof(DeviceSettings) <= MAX_PAYLOAD, ...)` —
`DeviceSettings` is sent whole in one 60-byte USB HID report for `GET_SETTINGS`/
`SET_SETTINGS`, and was already close to that ceiling (this exact constraint is why
`DEFAULT_TASK_UART_MS`/`DEFAULT_TASK_BME280_MS` etc. are fixed literals instead of
`DeviceSettings` fields, per their own comments — same constraint, not previously hit by a
field that needed genuine runtime calibration rather than a fixed literal). Backed out of
`DeviceSettings` and moved to the standalone-struct pattern described above instead, which
sidesteps the shared 60-byte budget entirely (each new struct gets its own page, sized to
just what it holds) and is arguably the better conceptual fit anyway —
`DisplacementSensorCal`/`DisplacementSharedCal` are calibration data, the same category as
`CalibrationData`, not instrument settings.

## Implementation

- `Math/math_phasor.c`/`.h` (new) — `math_phasor_accumulate()` (the Q14 I/Q accumulation
  primitive, moved out of WP8's `svc_signal_analysis.c` so it's a reusable Math/ building
  block) and `math_complex_div()`.
- `Config/config.h` — WP10 channel-mapping/derivation comment block, `DEFAULT_DISP_*`
  nominal defaults, `DISPLACEMENT_RING_DEPTH` (64), three new EEPROM page addresses/versions.
  `SIGNAL_ANALYSIS_BATCH_CYCLES` (WP8) removed along with its consumer.
- `system_state.h`/`.c` — `DisplacementSensorCal`/`DisplacementSharedCal` structs and their
  three global instances; `SystemState` gains `disp1_delta_mm`/`disp1_residual`/
  `disp2_delta_mm`/`disp2_residual`/`disp_ok` (latest-cycle snapshot only, see above).
- `Services/svc_storage.c`/`.h` — generic `svc_storage_save_blob()`/`load_blob()`, the three
  new pages wired into `svc_storage_init()`.
- `Services/svc_displacement.c`/`.h` (new) — the ISR/task-split pipeline described above,
  replacing `Services/svc_signal_analysis.c`/`.h` (deleted).
- `App/app_scheduler.c` — `task_signal_analysis` replaced with `task_displacement`, wired to
  `SYSTICK_PERIOD_MS` (every tick, not `task_sensors_ms`) for the reason above.
- `Core/Src/main.c` — `svc_signal_analysis_init()` call replaced with
  `svc_displacement_init()`.

**Build-verified clean — no CubeMX regen needed** (reuses WP8's existing ADC/DMA/timer
config as-is). Compiles and links with zero warnings.

## Code review (2026-08-18)

8 parallel angles (line-by-line diff scan, removed-behavior audit, cross-file tracer, reuse,
simplification, efficiency, altitude, CLAUDE.md conventions). Two real correctness bugs
found and fixed — one of them severe — plus four cleanup fixes; two lower-severity items
left as documented, deliberate tradeoffs:

- **Fixed, most severe: `svc_storage_load_blob()` wrote the EEPROM-read payload into the
  caller's buffer *before* the CRC/version check that determines whether the read is
  valid.** This exact bug class was already found and fixed once in this codebase, for
  `load_section()` — its own comment explicitly warns about it ("committing the read
  result before validation would let corrupt/mismatched EEPROM bytes overwrite a good
  default... the exact bug this ordering exists to prevent"). The new generic blob loader
  reintroduced it: a corrupted-but-magic-intact page would clobber the caller's
  already-seeded default in RAM, fail its own CRC check, and then
  `load_or_reseed_disp_blob()` — believing the data was still the good default — would
  write the *corrupted* value back to EEPROM with a freshly matching header, permanently
  laundering the corruption. Fixed to match `load_section()`'s pattern: read into a local
  temp buffer first, only `memcpy` into the caller's pointer on full CRC+version success.
- **Fixed: the output ring buffer would permanently freeze at the first ~24.6 ms of data
  produced after boot.** `push_output()` dropped new results once the buffer was full;
  since `svc_displacement_pop()` has no caller yet in this WP (by design — see "Not yet
  done"), the buffer fills within 24.6 ms of boot and then discards every subsequent result
  for the rest of the firmware's uptime. A future consumer added hours into a session would
  only ever see boot-time data, not anything recent — defeating the task's explicit
  requirement to retain the stream "so it can later be analyzed." Fixed by switching the
  output ring's overflow policy to overwrite-oldest (deliberately the *opposite* of the
  input ring's drop-newest policy, and of CLAUDE.md 8.3's comms-transport convention, which
  assumes an actively-reading peer) — it now always holds the most recent ~24.6 ms of
  results regardless of whether anything is currently draining it.
- **Fixed: a saturating counter written from ISR context lacked `volatile`.**
  `s_input_drop_count` is incremented in `on_sample()` (ISR) and read from a task-context
  getter, same crossing `s_in_head`/`s_in_tail` make — but unlike those, it wasn't
  `volatile`. Low practical severity (16-bit loads are atomic on Cortex-M0+, no polling
  loop reads it yet) but a latent bug matching the exact pattern the file's own comments
  warn about elsewhere.
- **Fixed (reuse): `load_or_reseed_disp_blob()`'s EEPROM reseed-on-failure sequence
  duplicated code already inline in `svc_storage_init()`'s `DeviceSettings` section loop.**
  Both did the identical `build_header` → `math_crc16` → `memcpy` → `blocking_write_block`
  → escalate-on-failure sequence. Extracted a shared `reseed_blob()` helper, used by both —
  the `svc_storage_save_blob()`/`load_blob()` generalization earlier in this same diff had
  covered the read path but left this write-path duplication only half-fixed.
- **Fixed (simplification): `process_one_cycle()` hand-duplicated the S1 and S2
  computations** instead of a small helper called twice, even though the shared `(A-B)`
  denominator was already factored out. Extracted `compute_sensor_delta()` — the module
  this replaces (WP8's `svc_signal_analysis.c`) already looped over channels for the same
  kind of repeated per-channel math, so the codebase's own precedent argued for this.
- **Fixed (simplification): the channel-mapping paragraph was duplicated verbatim** between
  `Config/config.h`'s comment and `Services/svc_displacement.c`'s top comment, while the
  derivation math one paragraph later in the same `config.h` block already used the better
  pattern of cross-referencing `svc_displacement.c` instead of repeating itself. Trimmed
  `config.h`'s copy to a cross-reference, matching that existing pattern.
- **Not applied**: a comment on `DISPLACEMENT_RING_DEPTH` claims its ~24.6 ms headroom
  "comfortably... with margin" covers WP9's worst-case scheduler stall, but a stuck/
  disconnected BME280 chaining three blocking I2C calls could in principle run somewhat
  higher than the single conversion-time figure the comment leans on — flagged as a
  documentation-precision concern, not resized without real measurement; the drop counter
  will surface it if the margin turns out insufficient. `math_phasor_accumulate()`'s
  `sample_idx` bounds check is unreachable given its single caller's usage pattern (~83k
  calls/sec in the ISR hot path) but costs an estimated ~0.1-0.2% CPU — left as defensive
  programming, consistent with this codebase's general style.

**Rebuilt clean after all fixes: compiles and links with zero warnings.**
RAM 39.7 KB (26.9%), FLASH 141.0 KB (26.9%).

## Code review, round 2 (2026-08-18)

Round 1's fixes were themselves new/changed code that hadn't been checked yet, so a second
full 8-angle pass ran against the fixed state before pushing. Four of the five angles
(line-by-line, removed-behavior, cross-file tracer, conventions) reported **zero new
findings** — every round-1 fix held up under independent scrutiny (parameter order, buffer
sizing, escalation paths, and a full clean rebuild all re-verified). The reuse/simplification
and efficiency/altitude angles found three worth fixing and two worth a deliberate,
documented "not now":

- **Fixed: `compute_sensor_delta()`'s 9-parameter list had 5 identical same-typed float
  arguments at both call sites**, with no compiler protection against a future edit
  transposing two adjacent ones (e.g. `iB`/`qB`). Bundled the 5 invariant values into a
  small local `SharedCycleTerms` struct built once in `process_one_cycle()` and passed by
  pointer to both calls.
- **Fixed: the input ring's inline drop-newest logic and the output ring's overwrite-oldest
  `push_output()` have a similar head/next/tail shape but opposite policies**, with nothing
  stopping a future refactor from copying the wrong one across if the input side is ever
  extracted into a named helper. Added an explicit comment at the input ring's eviction
  check warning against this — copying `push_output()`'s eviction branch there would move a
  tail-index write into ISR/producer context, violating the single-writer invariant the
  file's own header comment establishes for `s_in_tail`.
- **Fixed: the write-before-validate EEPROM-loader bug has now been independently introduced
  and fixed twice in this codebase** (`load_section()`, then `svc_storage_load_blob()` in
  round 1) — each fix documented only as a per-function comment, not somewhere a third
  loader implementer would actually see. Added a prominent, explicit warning at
  `load_section()` (cross-referencing `load_blob()`) for whoever adds the next one.
- **Deliberately not unified: `load_section()` still duplicates the logic
  `svc_storage_load_blob()` generalizes**, rather than delegating to it. Checked before
  declining: `svc_storage_load_blob()`'s internal buffer is sized to `sizeof(CalibrationData)`
  — verified via the actual ARM cross-compiler at **exactly 28 bytes** — and the scheduler
  `DeviceSettings` section's field data is **also exactly 28 bytes**, zero bytes of margin.
  Redirecting `load_section()` to call `load_blob()` today would work by coincidence, but
  silently break the instant any section gained one more byte of fields. Left as two
  separate implementations rather than introduce that landmine under review-passing time
  pressure; a real unification would need `load_blob()`'s buffer resized to
  `sizeof(DeviceSettings)` (already `load_section()`'s own bound) first.
- **Not applied**: `svc_displacement_pop()` having zero callers means the output ring's
  saturating drop counter reaches `UINT16_MAX` within ~25 seconds of every boot and never
  resets, so it's already uninformative by the time a real consumer might exist. This
  matches the established, codebase-wide "saturates rather than wraps; never cleared once
  nonzero" convention every other drop counter in this project already uses (e.g.
  `usb_tx_dropped_count`) — not a WP10-specific regression, so left as-is.

**Rebuilt clean after round 2's fixes: compiles and links with zero warnings.**
RAM 39.7 KB (26.9%), FLASH 141.0 KB (26.9%).

## Not yet done

- **Real-hardware verification** — same caveat as every prior WP: nothing here has touched
  real silicon. The phasor math and channel-mapping assumption are confirmed against the
  task spec and the user directly, but unverified against an actual differential-capacitor
  sensor head until flashed.
- **Angle conversion** — explicitly out of scope per the task spec; δ (displacement) is the
  final output of this WP.
- **Consumer for the retained per-cycle stream** — `svc_displacement_pop()` exists and is
  build-verified, and now correctly retains a rolling recent window regardless of whether
  anything drains it (see code review above), but nothing in this WP calls it yet; the
  actual pendulum-swing/higher-frequency analysis it was retained for is future work.
- **`SET_DISPLACEMENT_CAL`-style API command** — not built; the three new calibration
  structs are currently only loadable/saveable from within `svc_storage.c` itself (boot-time
  load, and `svc_storage_save_blob()` is available for a future caller), with no host-facing
  way to write new calibration values yet.
