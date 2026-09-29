# 1-hour PGA=1 drift comparison — findings

**Preliminary — based on live monitoring during the run, not a full
post-hoc statistical analysis of the CSV (no temperature-correlation
regression has been run on this dataset the way it was on the 24h
granite-plate one).**

## What was observed live

- Clean run throughout: 63,140 rows, zero sequence gaps.
- Onboard temp held flat (~27.0-27.2°C) for the whole hour — no thermal
  transient comparable to the 24h test's charging event, so this run alone
  can't directly test the "does drift track temperature" question the way
  the 24h test could. USB was disconnected throughout (no charging
  available), unlike the 24h test.
- S1/S2/diff means and step-to-step noise stayed in a similar ballpark to
  the 24h test's quiet (post-departure) periods — no dramatic difference
  in character observed at PGA=1 vs PGA=16 over this hour.
- `input_drop`/`output_drop` saturated in a similar pattern and on a similar
  timescale to the PGA=16 runs (both counters pinned by ~t=40min) —
  consistent with the already-established scheduler/LIVE-screen-redraw
  contention being the driver of those drops, not PGA gain.
- `clip_count`/`amplitude_fault_count` stayed at 0 throughout, as expected
  at the smaller PGA=1 full-scale.

## Open

Without a real thermal transient in this window (temperature was flat the
whole hour), this run doesn't yet give a clean answer to the original
question ("does the PGA=16 stage's temperature coefficient explain the 24h
test's drift?"). A repeat with either a real temperature excursion (e.g.
during a charging event) or a direct post-hoc correlation analysis against
whatever small temperature variation did occur would be needed for a firm
conclusion.
