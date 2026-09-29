# 24-hour granite-plate monitor — findings

Full run: 1,508,506 rows over ~24h (with three brief logged pauses for live
side-investigations), zero sequence gaps on both the raw-displacement and
phasor streams for the entire run. fw 0.10.53 throughout.

## Noise vs. the earlier 2-hour office-desk test

Granite is quieter in the typical/median sense but not obviously quieter in
big-jump rate. Step-to-step std over the first ~7.5min: S1 0.00247mm (vs
office desk's 0.00293mm baseline), S2 0.00460mm (vs 0.00526mm), diff
0.00274mm (vs 0.00304mm) — roughly 10-20% quieter across the board. But the
rate of large (>20µm) jumps was *higher* on granite in the same window (S1
0.139% of steps vs 0.028%, S2 1.14% vs 0.81%) — median fluctuation really is
in the single-micron range (matches the "front looks calm on the display"
visual impression), but occasional large excursions in the raw pre-MA stream
are not eliminated by a quieter surface, just usually invisible because the
LIVE screen shows the post-moving-average value.

## Noise drop when the operator left the room

Sharp, sustained transition at t≈180-195min: step std dropped from the
3-6.5µm range down to ~0.3-0.9µm and stayed there for hours. The differential
channel improved far more (8-10x) than either individual channel (1.5-3x) —
the signature of a *common-mode* disturbance (footsteps, talking, general
room vibration) that the differential scheme is specifically designed to
reject. Once the source left, both channels quieted, and the diff — already
built to cancel exactly this kind of shared noise — dropped almost an order
of magnitude.

## Natural charging event (t≈247min onward)

Battery crossed the charge-start threshold on its own (not forced) at
t≈247min. Produced a sharp ~70µm common-mode jump in S1/S2 (diff barely
moved), a secondary hump around t≈320-360min, then a long, slow relaxation.
Noise during active charging was 6-10x the pre-charging baseline on the
individual channels; the differential stayed essentially unaffected
throughout — same common-mode story, much sharper demonstration of it than
the earlier 2-hour test's forced-charge comparison (that one charged from
98% SoC, near-full and low-current; this was a natural mid-range charge, and
correspondingly noisier).

**Unresolved:** a large, non-common-mode spike around t≈1250-1280min (during
the charging tail) — diff mean jumped +0.036mm in one 30-min window and
briefly *crossed sign*, the only point in the whole run where the
differential itself moved dramatically instead of cancelling. Settled back
to a new (not the original) baseline over the following hour. No live
diagnostics were pulled at the time; cause not identified.

## Drift character

Diff mean drifted slowly and mostly monotonically downward for the first
~19 hours (roughly -0.04mm total), reversed for a few hours around the
temperature/charging-tail spike above, then resumed a slow decline. Not
well-explained by a simple temperature correlation over this run specifically
(see the correlation analysis below).

## Self-referencing / precision-measurement noise floor

Simulated a 15-minute self-reference (3s local average as the checkpoint)
with linear interpolation between checkpoints, scored against 3-second
"precision measurement"-style windows (matching the real triggered-precision
feature's own averaging):
- Whole-run residual std: 0.914µm (vs 6.854µm raw, ~7.5x reduction).
- **Steady-state (t=180-565min, the quiet post-departure period): 0.129µm,
  max deviation 2.1µm.** This is the number that matters for "how good can
  this get" — close to the ~0.10µm measured directly on the live post-MA
  value over a few seconds.
- Checkpoint interval barely matters (5 to 60 minutes all landed within
  2.78-2.84µm on the raw-sample comparison) — the diff's drift is smooth
  enough relative to the noise floor that reference frequency isn't the
  limiting factor.

## Board-side vs. sensor-side drift (correlation analysis)

- A/B (exciter) channels: essentially perfectly stable all day (0.07%
  range) — rules out the DDS/excitation path as a drift source.
- Onboard temperature correlates strongly with S1 (r=+0.77) and S2 (r=+0.57)
  individually, but ~0 with the differential (r=+0.002) — a shared,
  temperature-dependent, common-mode effect (best candidate: the ADC's own
  PGA=16 stage, since it's the one thing S1/S2 share that A/B don't).
- The diff's own slow drift does *not* correlate with temperature and the
  two sensors' raw amplitude wobbles are only weakly cross-correlated
  (r=+0.11, vs. their demodulated outputs at r=+0.88) — points to each
  sensor's own input-side path (cable/connector/capacitive head), not a
  shared board component, for the part that actually matters for precision
  measurement (the part that doesn't cancel).

## Not covered by this test

Instrument calibration (gain/zero-offset architecture, per-instrument sign
convention) was investigated and substantially reworked in a later session —
see `docs/wp10_displacement.md`'s entries from that date, not this folder.
