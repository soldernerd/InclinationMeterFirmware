# Design decisions and tuning history

Rationale that used to live in long comments in `Config/config.h`. Kept verbatim so no finding is lost; the code comments now carry only the facts needed to read the code. Dates are when the decision was made; values quoted here may since have changed (`git log` has the rest).

## Batch size, livelock root cause and margin history (was Config/config.h)

```c
/* ROOT-CAUSED 2026-09-24 with a real debugger session (STM32_Programmer_CLI
 * -halt/-coreReg/-r32 over SWD -- see docs/wp10_displacement.md for the
 * full walkthrough), after the earlier "known open issue" writeup that
 * used to sit here turned out to be chasing the wrong layer entirely.
 * The MCU was never actually crashed: uwTick (pure SysTick-ISR-driven,
 * independent of the scheduler) kept advancing normally throughout,
 * while app_scheduler.c's s_tasks[] showed task_displacement's
 * last_run_ms frozen from the moment it was first called -- i.e. the
 * scheduler's main loop was stuck *inside one call* to
 * svc_displacement_update(), specifically its `while (s_in_tail !=
 * s_in_head)` drain loop, and never returned.
 *
 * Why: the per-cycle complex-division math (3 float divisions +
 * surrounding multiplies, no hardware FPU on this Cortex-M0+) costs more
 * per cycle than the ~384 us a cycle takes to produce (2.6 kHz carrier).
 * Compute-only, that's already marginal -- bench-measured ~18% of
 * cycles dropped even with nowhere to store the result. Add the few
 * extra stores push_output()/the API snapshot need and the average
 * tips over budget: on_sample() (ISR context) queues new cycles into
 * s_in_ring faster than the drain loop can empty it, so the loop's own
 * exit condition can never become true -- a livelock, not a crash, and
 * not fixable by changing what gets stored (every earlier bisection
 * attempt that "fixed" it by removing storage was really just removing
 * enough per-cycle cost to stay under budget, not fixing a logic bug).
 *
 * Real fix, two parts:
 *  1. DISPLACEMENT_BATCH_CYCLES -- coherently sum this many consecutive
 *     cycles' raw I/Q (cheap int64 adds, same accumulation ISR-side)
 *     before running the expensive per-batch complex division once,
 *     instead of once per single cycle. Cuts the division-heavy work by
 *     this factor (and, as a bonus, is a longer coherent integration --
 *     better SNR, not just a workaround).
 *  2. DISPLACEMENT_MAX_CYCLES_PER_TICK (now DISPLACEMENT_MAX_BATCHES_PER_TICK,
 *     2026-10-03, below) -- defensive cap on how many raw
 *     cycles svc_displacement_update() will dequeue in one call,
 *     regardless of backlog, so a future transient overload (scheduler
 *     jitter, a slow tick elsewhere) can degrade to dropped cycles
 *     (already-proven-safe, graceful) instead of ever livelocking the
 *     scheduler again -- same bounded-pump shape as
 *     DISPLAY_PAGES_PER_TICK / the retired bulk-chunk pump.
 *
 * MARGIN HARDENED 2026-09-25: 8/32 gave board 1 a clean 242 s zero-drop
 * soak, but board 2 -- identical hardware, ordinary component/clock
 * tolerance, no defect found or expected -- measurably drops cycles in
 * bursts at the same settings (confirmed even on firmware predating any
 * of that day's other changes; see docs/wp10_displacement.md's "Board 2's
 * timing margin" section). Two boards behaving differently at a setting
 * this close to its own documented ~18%-marginal budget means the
 * budget was too thin to begin with, not that board 2 is faulty -- the
 * fix is the same lever that solved the original livelock, pushed
 * further: quadrupling DISPLACEMENT_BATCH_CYCLES to 32 cuts the
 * division-heavy work rate to a quarter (~81 updates/s, still ample for
 * a mechanical displacement reading -- WP8's old signal-analysis module
 * ran its own per-batch finalize at just 40 Hz), and doubling
 * DISPLACEMENT_MAX_CYCLES_PER_TICK (since replaced by
 * DISPLACEMENT_MAX_BATCHES_PER_TICK, see below) alongside the doubled
 * DISPLACEMENT_RING_DEPTH above keeps recovery-from-backlog just as fast
 * proportionally. Needs the same re-verification the original fix got
 * (a long soak watching input_drop_count) before being trusted as
 * "enough" margin, not just "more" margin.
 *
 * RE-VERIFIED 2026-09-26: NOT enough on its own. A fine-grained
 * (200 ms) sampler on input_drop_count (rather than the coarse
 * before/after snapshots used previously) found board 2 still drops a
 * real, regular ~500-cycle burst every LIVE_DISPLACEMENT_REFRESH_MS
 * period (App/app_display.c) -- present at BOTH 32 and 128 cycles/batch
 * (see that constant's bump comment below), so the LIVE screen's redraw
 * cost, not the division rate this constant controls, is what's actually
 * eating the margin at rest. This constant's own fix (cutting division
 * work) is real and unregressed; it just isn't the dominant term here.
 * See App/app_display.c's LIVE_DISPLACEMENT_REFRESH_MS comment for the
 * full writeup -- the redraw-cost bug itself is still open. */
/* BUMPED 32 -> 128 2026-09-26, at the user's request, after the noise
 * investigation (bulk-capture spectral analysis, see docs/wp10_displacement.md)
 * found the sensor channels' noise floor -- not harmonics, not digital
 * coupling -- to be the dominant limit on measurement quality. Same lever
 * as the earlier margin-hardening above, pushed further for two stacked
 * benefits instead of just headroom:
 *  1. Cuts the division-heavy per-batch work another 4x (~20.3 batches/s
 *     now, 2604.167/128 -- still the userspace-visible getters, nothing
 *     needs faster than that), freeing CPU margin -- though bench-testing
 *     the same day found this specific margin isn't what was gating
 *     LIVE_DISPLACEMENT_REFRESH_MS (App/app_display.c); see the
 *     "RE-VERIFIED 2026-09-26" paragraph above and that constant's own
 *     comment for the actual (still open) bottleneck.
 *  2. A longer coherent (pre-division) integration window is a real SNR
 *     gain on its own: sqrt(128/32) = 2x (~6 dB) over the previous
 *     setting, stacking with the post-division moving average below
 *     (DISPLACEMENT_MA_SAMPLES) for roughly sqrt(4)*sqrt(4) = 4x (~12 dB)
 *     total over the pre-2026-09-26 setup.
 * DISPLACEMENT_ZERO_CAL_SAMPLES below was rescaled 128->32 alongside this
 * so its total raw-cycle integration depth (samples * batch cycles) and
 * wall-clock duration per step are unchanged -- see its comment.
 *
 * BACK OFF 128 -> 64 2026-09-27, at the user's request, to test whether
 * finer batches + more post-division averaging beats fewer/coarser batches
 * for the differential/precision-measurement strategy (docs/wp10_displacement.md
 * has the full reasoning): division is effectively linear at this system's
 * operating point (x only ever sits ~1e-5..1e-6 away from 0.5), so a big
 * single coherent batch and many smaller batches averaged afterward reach
 * the same noise floor for the same total integration time -- the real
 * difference is that smaller batches let the quality flag
 * (DISPLACEMENT_QUALITY_BAD_MULTIPLE below) detect and exclude a transient
 * at finer time resolution instead of having it silently blended into one
 * bigger division. DISPLACEMENT_MA_SAMPLES below was doubled 4->8 alongside
 * this specifically so the smoothed (post-MA) value's total SNR is
 * UNCHANGED: sqrt(64)*sqrt(8) = sqrt(512) = sqrt(128)*sqrt(4) exactly -- the
 * live/smoothed reading doesn't get worse, only the exclusion granularity
 * gets finer (2x). DISPLACEMENT_ZERO_CAL_SAMPLES doubled 32->64 alongside
 * this too, same "keep total raw-cycle depth constant" reasoning as its own
 * 2026-09-26 rescale. Bench comparison against the 128-cycle setting still
 * pending -- this is explicitly an experiment ("let's see if this makes
 * things better"), not a settled tuning. */
```

## Sensitivity (D0_UM, GAIN_MILLI) bumps of 2026-09-26 -- superseded by the k/zero model of 2026-10-06 (was Config/config.h)

```c
/* Nominal calibration seeds (DeviceSettings' displacement page, EEPROM-
 * backed past first boot — see system_state.h's comment on those
 * fields). Scaled integers, not raw floats, to fit svc_api.c's
 * integer-only SF() field machinery: milli-units (x1000) for the two
 * dimensionless ratios (atten, gain), micrometers for the two lengths
 * (d0, zero_offset). Nominal atten=3 matches the board's actual A/B
 * attenuator (bench-confirmed, 2026-09 RC-filter investigation); gain
 * and d0 are un-bench-calibrated starting points, same "unconfirmed
 * against real hardware" caveat as DEFAULT_ENCODER_COUNTS_PER_DETENT
 * above. zero_offset starts at 0 (no bench zero calibration done yet).
 *
 * D0_UM BUMPED 1000x 2026-09-26 (100 -> 100000), THEN A FURTHER 7x
 * (100000 -> 700000, ~7000x total from the original 100) THE SAME DAY --
 * a deliberate, coarse "get in the right ballpark" sensitivity fix, at
 * the user's request. The first 1000x was a rough estimate ("probably
 * 1000 times too low, maybe even more"); the 7x followed a real physical
 * check -- a single 80 g/m^2 paper sheet (~100 um caliper) as a known
 * shim under one end over a known baseline -- comparing the resulting
 * reading to the expected displacement ("multiply sensitivity by 7x from
 * here and it's about right"). compute_sensor_delta()
 * (Services/svc_displacement.c) computes delta_mm = 2*d0_mm*(x-0.5) -
 * zero_offset_mm -- d0 is a pure linear scale factor on the FINAL output
 * only, with zero effect on x, residual, or the degenerate-denominator
 * check (unlike gain, which sits inside x's own computation and also
 * feeds the shared A-B reciprocal), which is exactly why it's the
 * lever used here: a clean, isolated multiplier, not a hack tangled up
 * in the demod math. This is NOT a claim that the sensor's real
 * mechanical air gap is 700 mm -- it's standing in for the still-missing
 * real gain calibration (the bulk-capture signal-quality pass separately
 * found the nominal gain=10 doesn't match either board's actual
 * hardware, real gain closer to ~0.13 -- a mismatch in the same rough
 * ballpark once the S-channel's real-vs-assumed attenuation is folded in
 * too, though the paper-shim check is the more trustworthy number since
 * it's an actual physical measurement, not a component-tolerance
 * estimate). Replace with a proper gain/d0 bench calibration against a
 * real reference when one is done -- see docs/wp10_displacement.md's
 * "Current status" deferred list.
 *
 * GAIN_MILLI BUMPED 16x 2026-09-26 (10000 -> 160000), alongside setting
 * the ADS131M04's own PGA to 16 on the S1/S2 channels only
 * (Drivers_App/drv_ads131m04.c's GAIN1_REG_VALUE). This `gain` constant
 * models an analog stage AHEAD of the ADC (external pre-amp); the ADC's
 * PGA is a SEPARATE, independent gain stage after it. Since A/B stay at
 * PGA=1 (unscaled) while S1/S2 now read back 16x larger raw codes for
 * the same physical signal, compute_sensor_delta()'s x=(S/k-B)/(A-B)
 * (k=atten*gain) needs `gain` scaled by that same 16x to cancel the ADC's
 * own amplification back out -- otherwise x (and therefore delta_mm)
 * would silently read 16x too small. This is NOT a re-calibration of the
 * real analog gain (still ~0.13 per the bulk-capture signal-quality
 * pass referenced above) -- it's compensating for a hardware change,
 * same shape as D0_UM's bumps above but for a different reason. */
```

## Zero-calibration sample count rescaling (was Config/config.h)

```c
/* --- Displacement zero calibration (180-degree reversal test, 2026-09-25) ---
 * Standard precision-level technique: place the instrument, average its
 * reading (step 1), physically rotate it 180 degrees IN PLACE, average
 * again (step 2). A real surface tilt phi contributes with opposite sign
 * to each step's reading (the instrument's own frame flips relative to
 * the surface), while the instrument's own zero error stays the same
 * (it's intrinsic to the sensor, not the surface) -- so the two steps'
 * average cancels phi and isolates the zero error:
 *   step1 = phi + zero_error, step2 = -phi + zero_error
 *   zero_error = (step1 + step2) / 2
 * Applied independently per sensor head (S1, S2 each have their own
 * disp_*_zero_offset_um -- see svc_displacement.c's
 * svc_displacement_zero_cal_*() and Services/svc_api.c's Commands
 * API2_RES_CMD_ZERO_CAL). Per-instrument by construction: this only ever
 * reads/writes THIS device's own g_device_settings and its own EEPROM --
 * nothing here is shared across physical units.
 *
 * Samples averaged per step. At the original DISPLACEMENT_BATCH_CYCLES
 * (32, ~81 batches/s), 128 samples =~ 1.6 s per step -- enough averaging
 * to ride out ordinary sensor noise (see the bulk-capture signal-quality
 * analysis) without making the user hold the instrument still for long.
 *
 * RESCALED 128 -> 32 2026-09-26 alongside DISPLACEMENT_BATCH_CYCLES'
 * 32->128 bump: the batch rate dropped 4x (81 -> 20.3 Hz), so this is
 * divided by the same 4x to keep both the wall-clock duration per step
 * (~1.6 s, unchanged) AND the total raw-cycle integration depth (samples
 * * batch cycles = 32*128 = 4096, identical to the old 128*32) exactly
 * where they were -- not a re-tuning, just following the batch size.
 *
 * RESCALED 32 -> 64 2026-09-27 alongside DISPLACEMENT_BATCH_CYCLES'
 * 128->64 halving -- same reasoning, opposite direction: batch rate
 * doubled (20.3 -> ~40.7 Hz), so this doubles too, keeping the wall-clock
 * duration per step (~1.6 s) and total raw-cycle depth (64*64 = 4096)
 * unchanged. */
```

## EEPROM displacement-page version history (was Config/config.h)

```c
#define EEPROM_DISPLACEMENT_SETTINGS_VERSION 0x000C  /* 0x0002 (2026-09-26): DEFAULT_DISP_S1/
                                                         S2_D0_UM bumped 1000x (sensitivity
                                                         fix, see that comment) -- learned
                                                         from the battery-scale bug earlier
                                                         this session (memory
                                                         battery-voltage-calibrated) that a
                                                         DEFAULT_* change alone does NOT
                                                         reach a board whose EEPROM page was
                                                         already written under the old
                                                         version; bumping here so this
                                                         propagates on next reflash instead
                                                         of needing a manual SET per board.
                                                         0x0003 (same day): a further 7x on
                                                         D0_UM after a real paper-shim check
                                                         (~7000x total from the original
                                                         default) -- same propagation
                                                         reasoning, bump every time the
                                                         default changes, not just the
                                                         first time.
                                                         0x0004 (same day): DEFAULT_DISP_S1/
                                                         S2_GAIN_MILLI bumped 16x (10000 ->
                                                         160000) to compensate for the
                                                         ADS131M04's own PGA going to 16 on
                                                         S1/S2 (Drivers_App/drv_ads131m04.c) --
                                                         same propagation reasoning again.
                                                         0x0005 (2026-09-27): DISP_S1/S2_D0_UM
                                                         fields REPLACED by D0_THEORETICAL_UM +
                                                         CAL_MULT_MILLI (struct layout change,
                                                         not just a default-value change --
                                                         see DEFAULT_DISP_S1_D0_THEORETICAL_UM's
                                                         comment for the Wyler-handbook
                                                         derivation); defaults chosen to
                                                         reproduce the old d0=700mm exactly, so
                                                         this bump changes field layout, not
                                                         behavior.
                                                         0x0006 (2026-09-29): disp_s1_invert/
                                                         disp_s2_invert ADDED (struct layout
                                                         change, appended at the end of the
                                                         displacement section) -- per-instrument
                                                         sign flip on the FINAL reported reading,
                                                         applied at the svc_displacement.c getter
                                                         layer so it can't disturb the existing
                                                         gain/zero-cal math (see system_state.h's
                                                         comment). Defaults to 0 (not inverted)
                                                         on both, so this bump is layout-only,
                                                         same as 0x0005.
                                                         0x0007 (2026-09-29, same day): zero_offset_um's
                                                         MEANING changed -- was the final output-mm
                                                         domain (root cause of instruments reading
                                                         several mm off at true level right after a
                                                         cal_mult change, twice in one session), now
                                                         the cal_mult-INDEPENDENT theoretical domain
                                                         (system_state.h's disp_s1_zero_offset_um
                                                         comment has the full derivation) -- one
                                                         zero-cal run now stays valid across future
                                                         cal_mult changes. No struct layout change,
                                                         but old on-disk values would be silently
                                                         misinterpreted under the new meaning, so this
                                                         still needs a version bump to force a reset
                                                         to 0 rather than reusing stale data under a
                                                         different domain.
                                                         0x0008 (2026-09-29, same day): cal_mult_milli
                                                         RENAMED to sensitivity_uv_per_um_milli --
                                                         same storage slot, but reinterpreted from a
                                                         bare dimensionless ratio (e.g. "2.669x") to
                                                         the sensor's real uV-per-0.001mm/m sensitivity
                                                         (e.g. "7493" = 7.493 uV/um/m) -- directly
                                                         comparable to the 20uV nominal spec, no mental
                                                         unit conversion (system_state.h's field
                                                         comment has the full reasoning). A stale
                                                         dimensionless-ratio value under the new
                                                         interpretation would be silently, badly wrong
                                                         (2.669 read as "2.669 uV/um/m" is nowhere near
                                                         2669), so this bump forces a reset to the
                                                         nominal-spec default (20000) rather than
                                                         reusing it under the new meaning.
                                                         0x0009 (2026-10-06): disp_s1/s2_phase_cdeg
                                                         ADDED (struct layout change) -- the phase
                                                         calibration of docs/signal_processing.tex
                                                         Sec. 9.1. Reseeds the whole displacement
                                                         page to the DEFAULT_DISP_* values, so
                                                         gains, sensitivities, zero offsets and the
                                                         invert flags must be set again (the zero
                                                         and sensitivity calibrations were due for a
                                                         redo anyway).
                                                         0x000A (2026-10-06): disp_atten_milli REMOVED
                                                         (layout change) and the gain defaults x3
                                                         (160000 -> 480000): |k| is now one empirical
                                                         number per sensor. Page reseeded again.
                                                         0x000B (2026-10-06): |k| now stored for PGA = 1
                                                         (default 480000 -> 30000; the firmware multiplies
                                                         by the PGA). Same layout, new meaning, so the
                                                         page is reseeded rather than misread 16x.
                                                         0x000C (2026-10-06): REDESIGN -- d0_theoretical and
                                                         sensitivity removed; per sensor one k (x1e-6, PGA 1,
                                                         default 21300) and one zero (ppm of the ratio, k-
                                                         independent). Layout and meaning change. */
```
