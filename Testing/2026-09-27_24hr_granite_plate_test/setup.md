# 24-hour granite-plate monitor

**Date started:** 2026-09-27. **Firmware:** 0.10.53. **Board:** whichever board was
on the bench when this was launched (`IDENTITY` serial `6DA08073`, COM5 — an ST-Link
VCP, VID:PID 0483:3754). USB connected throughout for charging as needed; no forced
charge/discharge cycling in this run (unlike the 2-hour test).

## Purpose

Same kind of unattended monitor as `Testing/2026-09-27_2hr_charging_drift_test/`, but
on the actual granite surface plate (a real, stable mechanical reference) instead of
an office desk, and extended from 2h to 24h to see longer-timescale drift/noise
behavior. Also records the raw phasors (Topic 0x5/0x02) and the full set of
acquisition/demod error terms continuously, which the 2-hour test did not.

## Method

`granite_plate_24hr_test.py`. Per `Testing/README.md`'s data-format convention, this
writes ONE flat CSV (`data/granite_24hr.csv`) instead of splitting fast/slow fields
across files with independent clocks:

- Row cadence is driven by the raw-displacement subscription (Topic 0x5/0x03,
  50 ms — ~20 Hz effective, same rate as the 2-hour test). Kept at 20 Hz rather
  than throttled down, at the user's explicit request ("we want to see
  everything and data volume isn't that great") — expect roughly 800MB-1GB for
  the full 24h.
- The phasor subscription (Topic 0x5/0x02, also 50 ms) and the slow GET-polled
  fields update asynchronously and are forward-filled into whichever row is
  emitted next.
- Slow channel, polled every 30s: onboard temp, battery mV/SoC/usb/charging
  (Topic 0x5/0x01), the displacement diagnostics (Raw 0x7/0x02 — clip_count,
  amplitude_fault_count, input/output_drop, max_update_gap_ms,
  gap_over_threshold_count), the ADS131M04 acquisition integrity tail (Raw
  0x7/0x00 — frame_deficit, framing/crc errors, ring backlog), and the signal
  diagnostics (Topic 0x5/0x04 — per-channel RMS/phase + the Wyler theoretical
  tilt estimate, the same numbers the DIAGNOSTICS screen shows).
- Console `STATUS` line at t=5min, then every 30 min after — the interval the user
  asked to be updated at. A background Monitor watches this process's stdout for
  `STATUS`/`WARNING`/`ERROR`/`DONE`/`Traceback` lines and surfaces each as a chat
  notification.

## Known data caveats, present from the moment this run started

The device's demod was already running (not freshly restarted) from earlier
today's bench work, so its saturating diagnostic counters carried over:
**`clip_count`, `input_drop`, AND `output_drop` were all pinned at 65535
(uint16 max) by the time the 20Hz run below started** — this run cannot show
any *further* clip or drop events on those three counters; treat them as
"already saturated," not "zero during this test." `amplitude_fault_count`
(625 at launch) and `gap_over_threshold_count` (8193 at launch) still had
headroom to grow further. Not fixed here (would mean restarting the demod,
which could disturb the physical granite-plate setup) — see
`docs/wp10_displacement.md`'s note that these should eventually be widened to
`uint32_t`.

## Status

Launched 2026-09-27 at 2Hz, restarted within the first few minutes at 20Hz
(user wanted full resolution — data volume isn't a constraint here). Ran the
full 24h (with three brief, logged pause/resumes for live side-investigations —
see the `event` column), completed cleanly: 1,508,506 rows, zero sequence gaps
across the entire run. See `findings.md`.
