# Contiguous 64-cycle phasor capture (planned 2026-10-04)

**Firmware:** 0.10.64 (built 2026-10-03, **not yet flashed** -- the board was not attached
to the build PC; the ST-Link read 0.00 V). Changes vs 0.10.62: `DISPLACEMENT_PHASOR_LOG_DECIMATION`
2 -> 1 in `Config/config.h` (store every batch, 0.10.63) and the per-position int32 batch
accumulation in the demodulation hot path (0.10.64; bit-identical I/Q, see
`docs/wp10_displacement.md`) -- flash this one and compare the readings/counters against 0.10.62.
Flash with `powershell -ExecutionPolicy Bypass -File flash.ps1` (build is in `build/Debug`).

## Purpose

Every earlier long-term log of the displacement readings was a decimated stream (~17 Hz
snapshots of a 40.7 Hz batch series, which aliases the ~20 Hz pendulum into noise) or 0.3 s
raw-ADC captures. The averaging/smoothing design (see
`Testing/2026-09-30_bulk_adc_30s_interval/findings.md`) needs the **contiguous** 40.7 Hz
series of 64-cycle phasors. Questions this data should answer:

* repeatability vs window length (0.5-4 s) for a **contiguous** boxcar vs triangular / Hann /
  cascaded boxcars (is the ~2x daytime gain real?)
* does excluding batches (median / trim / the residual-step flag) help or hurt on contiguous data?
* the pendulum line properly resolved (12.6 s segments -> 0.08 Hz resolution): frequency, Q,
  stability, and how well a "vibration" indicator (2nd difference of the batch values) tracks it
* how much the redraw-induced cycle drops break contiguity (the `gap_cycles` column)

## Method

`phasor_capture.py` stops the demod, then repeatedly runs the API Bulk phasor log
(`START_BULK`, category 0x8 / resource 0x01): 512 batches = 12.6 s contiguous per capture,
transferred over the wired UART (ST-Link VCP; `--port auto`). Captures run back-to-back
(or every `--interval-s`). One flat CSV, one row per batch, with `seq`, a `gap_cycles`
contiguity column (device-side dropped cycles show up as a `seq` jump), `first_batch` flag
(drop it: stale first ADC sample), and temp / SoC / charging polled once per capture.

Auto power-off: the timer only pauses for USB / BLE / charging (not for a wired-UART session or a
bulk capture), so the script sets `auto_poweroff_s` to 0 for the run and restores it at the end
(`--keep-autopoweroff` to leave it alone). If the script is killed hard the setting stays 0 --
restore it from the SETTINGS screen or API Settings 0x1B.

Self-check: the script aborts if consecutive batches are not 64 cycles apart (i.e. the
board still has the old decimation-2 firmware).

## Suggested runs for tomorrow

1. Flash 0.10.63; smoke test: `python phasor_capture.py --duration-min 0.5`, then
   `python first_look.py data/<file>.csv`.
2. **Quiet/reference:** 10 min, nobody walking nearby: `--duration-min 10 --label quiet`.
3. **Disturbed:** 10 min with someone walking / working nearby (the daytime case):
   `--duration-min 10 --label walking`.
4. Optional longer sparse run: `--duration-min 120 --interval-s 60 --label day`.

Keep the instrument on one screen (note which) -- the LIVE-screen redraw is known to drop
~500 cycles per refresh and will split the series into short segments (visible in `gap_cycles`).

## Files

* `phasor_capture.py` -- the capture routine
* `first_look.py` -- first analysis (quality, spectrum, strategy comparison, graph)
* `selftest_mock.py` -- hardware-free test of both (simulated device; its numbers mean nothing)
* `data/` -- captures (to be created)

## Status

Routine written and verified against a simulated device only (CSV format, gap detection,
first-batch handling, analysis run). **Not yet tested on hardware.**

## Update 2026-10-04 evening: gapless stream (fw 0.10.65) -- supersedes the bulk routine

The back-to-back bulk captures above leave a ~2.6 s hole every 12.6 s (UART transfer while
no capture runs, ~83 % duty). fw 0.10.65 adds API Topic 0x05 / resource 0x05, an
**event-driven stream of every 64-cycle batch** (FIFO of 64 entries in `svc_displacement`,
one frame per batch, ~1.75 kB/s on the wired UART). `phasor_stream.py` logs it:
`python phasor_stream.py --duration-min 1440 --label day24h`.

* 1-minute smoke test on hardware: 2443 batches in 60.0 s (40.71/s, ideal 40.69/s), every
  step exactly 64 cycles, 0 gap cycles, 0 lost frames, 0 bad frames.
* CSV columns: `cycles` (unwrapped device cycle count, exact time = cycles/2604.1667 s),
  `gap_cycles` (device-side loss, multiple of 64), `frame_gap` (wire-side loss),
  `first_batch` (drop it), slow fields polled every 60 s.
* `auto_poweroff_s` left at 0 (device never powers off). Charging is left to the firmware,
  but the script inhibits it (Commands 0x09) after 3 h of continuous charging
  (`--max-charge-h`) and clears the inhibit when the script exits.
* `data/` is git-ignored for this test (CSVs are large and not committed).
