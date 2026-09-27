# 10-minute streaming monitor

**Date:** 2026-09-26. **Firmware:** 0.10.42. **Board:** #2.

## Purpose

Redo of `../2026-09-26_10min_polling_monitor_v1/`'s jump characterization,
fixing that run's two flaws: undersampling and reading the post-smoothing
value. Needed a real subscribable raw stream first — this test is also the
bench validation of that new API feature (Topic 0x03, `svc_displacement`'s
pre-moving-average `delta1/2_mm_raw`).

## Method

`monitor_10min_stream.py` — proper `SUBSCRIBE` to Topic 0x03 (raw displacement,
~50ms interval floor, effective ~17-20Hz given batch timing) for 600s, using a
`Session` class (`_pump()`/`request()`/`drain_stream()`) to keep the transport
alive and drain pushed frames concurrently with any request/response traffic.
Slower-changing diagnostics (temperature, clip/fault counters, drop counters)
logged separately at a lower rate into `monitor_10min_stream_slow.csv` (two
files with independent `t_s` clocks — see the repo-level convention note in
`Testing/README.md` for why new tests should avoid this split going forward).

Two output files:
- `monitor_10min_stream.csv` — fast stream: `t_s,issue_seq,delta1_mm_raw,residual1,delta2_mm_raw,residual2`
- `monitor_10min_stream_slow.csv` — slow side-channel: temp, clip/fault/drop counters, `disp_ok`

Same board, same physical setup as the v1 polling run (not yet moved/swapped).
