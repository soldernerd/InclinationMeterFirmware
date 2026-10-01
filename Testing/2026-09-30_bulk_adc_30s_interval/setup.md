# Periodic raw-ADC bulk capture (every 30s)

**Date started:** 2026-09-30. **Firmware:** 0.10.62. **Board:** serial
6DA08073, COM5.

## Purpose

An alternative to the continuous demod-stream long-term tests
(`Testing/2026-09-27_24hr_granite_plate_test/`): instead of logging the
real-time displacement output, periodically capture the full-rate
(20833.33 Hz) raw ADC codes for all 4 channels via the existing Bulk
transfer feature (API category 0x8, `svc_displacement_capture_begin()`),
giving the most granular possible view of the raw signal at regular
intervals, at the cost of not having a continuous record in between.

## Why not continuous + bulk together

Bulk capture and the real-time displacement demod are mutually exclusive
by firmware design (`svc_displacement_is_running()` gate in
`Services/svc_api.c`'s `dispatch_bulk()`) -- not just a performance
guideline. True simultaneous continuous-stream + periodic-bulk isn't
possible without a firmware change. The demod is stopped once at the
start of this script and left stopped for the whole run.

## Method

`bulk_adc_30s_test.py`. Every 30s: one bulk raw-ADC capture (6144 samples
x 4 channels, ~295ms to acquire), drained over the wired UART (~6-7s at
115200 baud -- the drain, not the acquisition, is the bottleneck),
appended to a single growing binary log (`data/bulk_adc_log.bin`).

**Binary format**, chosen over CSV text for size (CSV would be ~3x
larger for the same data -- see the file's own docstring for the exact
layout): one record per capture, no padding, little-endian --
`float64 t_s, uint32 capture_idx, uint32 sample_count, uint32 gap_count`,
followed by `sample_count * 12` bytes of raw ch0-3 samples (3-byte LE
signed each, matching the on-wire/RAM layout exactly).

Planned duration: 24h (2880 captures, ~212MB). **The data file is
intentionally not committed** -- `*.bin` is already covered by
`.gitignore`'s blanket binary-file rule, consistent with how the 24h
granite-plate test's oversized raw CSV was excluded.

## Status (as of this commit)

Still running. 1141+ captures so far, zero gaps, zero truncated captures.
One early transient noted (first capture's S2 RMS was an outlier,
settling immediately to a stable baseline by capture 1 -- likely residual
thermal/electrical settling from the flurry of reflashes immediately
before this test started, not a real finding). `findings.md` to be
written once the run completes.
