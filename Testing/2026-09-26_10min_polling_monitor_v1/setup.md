# 10-minute polling monitor (v1 — superseded)

**Date:** 2026-09-26. **Firmware:** 0.10.41. **Board:** #2.

## Purpose

User reported LIVE readings look steady (±0.001-0.002mm) then occasionally
jump 0.01-0.02mm, and asked for a 10-minute unattended monitor to characterize
the jumps ahead of a granite-plate calibration session.

## Method

`monitor_10min.py` polled `Measurements` (0x4) delta1/delta2/residual1/residual2
plus `Raw data` 0x7/0x02 diagnostics repeatedly over 600s. Intended ~0.4s/sample;
actual rate was ~1.6s/sample (UART round-trip overhead dominated). `auto_poweroff_s`
was raised beforehand so the board wouldn't sleep mid-session (see
[[board2-standby-during-bench]]) and restored after.

## Known flaws — why this run is superseded

Two problems, both caught by the user:

1. **~1.6s/sample badly undersampled** the ~20.3 Hz source signal.
2. **Read the POST-moving-average value**, not the raw per-batch displacement —
   smoothing had already removed most of the fast structure being investigated.

This led directly to building a real `SUBSCRIBE`-based raw displacement stream
(Topic 0x03, ~20.3Hz) and repeating the same test — see
`../2026-09-26_10min_streaming_monitor/`. Kept here for completeness / as a
cautionary example of a measurement-method artifact.
