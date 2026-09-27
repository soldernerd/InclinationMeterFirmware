# Findings

**Streaming worked as intended:** 10275 synchronized samples over 10 minutes,
~17Hz effective rate, **zero sequence gaps** (`issue_seq` fully contiguous) —
first proof the API stream is solid enough to trust for granular analysis, not
just the demo case.

**The v1 polling run's "S1/S2 jumps never overlap" conclusion was wrong.**
With genuinely simultaneous samples: Pearson correlation between the full
detrended S1/S2 traces is **0.58** (real, moderate positive correlation), and
**100% of S2's big jumps (14/14, threshold >0.02mm) coincide exactly with an
S1 jump at the same sample** — there IS a shared component after all.

**But S1 also has a large population of jumps S2 doesn't share**: 284 total S1
jumps (>0.02mm) vs only 14 shared ones. S1's noise isn't clean two-state
"popcorn"/RTS switching — it's a continuous, heavy-tailed distribution (14.4%
of all S1 steps exceed 10µm, up to 65µm), more consistent with a physical/
mechanical noise source than semiconductor noise. No dominant periodic
frequency in an FFT of the S1 trace — aperiodic, not a resonance or
line-frequency artifact.

`clip_count`/`amplitude_fault_count` stayed flat throughout (from the slow
side-channel) — again rules out ADC clipping or a computation fault as the
cause.

**At the time, S1 was (wrongly) documented as "the sensor with an external
cable" and cable microphonics was the leading suspect** — this was later
corrected: both S1/S2 use identical, interchangeable 2m cables, and the swap
test (`../2026-09-26_sensor_swap_test/`) proved the noise follows the physical
sensor head, not the cable or connector. This folder's data itself doesn't
distinguish those two hypotheses; the swap test is what settled it.
