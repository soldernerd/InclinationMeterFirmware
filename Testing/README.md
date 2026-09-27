# Testing — bench test archive

Permanent record of every non-trivial bench test run against real hardware (as
opposed to a one-off diagnostic command whose output was only ever useful in the
moment). Each subfolder is one test, named `YYYY-MM-DD_short_description/`, and
contains, where applicable:

- `setup.md` — what was tested, why, exact firmware version, hardware
  configuration, and the method (so it's reproducible without re-reading the
  original conversation).
- `findings.md` — the interpretation: what the data showed, what it ruled in/out,
  and what's still open.
- `*.py` — the actual script(s) run to collect the data, unedited.
- `data/*.csv` — the raw data collected, unprocessed.
- `graphs/*.gif` — the plots generated from that data.

This exists because bench test results are easy to lose track of once a
conversation scrolls past them, and re-running an old test from scratch is far
more expensive than reading a two-year-old CSV. When in doubt about whether a
past finding still holds, check here before re-testing.

## Index

| Folder | What | Firmware | Headline finding |
|---|---|---|---|
| `2026-09-25_gain_sizing_bulk_capture` | Bulk raw-ADC capture at PGA=1 and PGA=16 | 0.10.41 | Confirmed PGA=16 gain change landed exactly (16.00x on S1/S2, A/B unchanged) |
| `2026-09-25_noise_spectrum_investigation` | FFT/THD spectrum analysis with digital subsystems on/off | 0.10.35 | Harmonics/digital switching are NOT the dominant noise source; broadband, not narrowband |
| `2026-09-26_10min_polling_monitor_v1` | First 10-min drift/jump monitor, via polled `GET` | 0.10.40 | Superseded — polling only achieved ~0.6Hz and read the post-smoothing value; see the streaming version below |
| `2026-09-26_10min_streaming_monitor` | Same monitor, redone via a real `SUBSCRIBE` stream | 0.10.42 | ~17Hz, zero dropped frames; found S1 (ch3) far noisier than S2 (ch0) |
| `2026-09-26_sensor_swap_test` | Same monitor with the two physical sensor heads swapped between connectors | 0.10.42 | Noise follows the physical sensor unit, not the cable/connector |
| `2026-09-27_2hr_charging_drift_test` | 2-hour run: 60min baseline + 60min forced charging | 0.10.49 | Charging raises noise 32-39%; drift does not reduce to a simple temperature coefficient |
| `2026-09-27_board2_vbat_calibration` | Board 2 (green) battery-ADC calibration against a Keysight PSU + DMM | 0.10.53 | Old calibration under-read by ~15-40mV; new fit sub-mV accurate 3.5-4.2V; 3.0V causes a real, non-self-recovering brownout |

See `docs/wp10_displacement.md` in the repo root for the full narrative writeup of
each finding in project context; this folder is the raw evidence behind it.

## Data format convention (for new tests)

New data-logging scripts should write a single flat CSV — one row per record, one
column per quantity — rather than splitting fast/slow-changing fields across separate
files with independent clocks (some of the earlier tests below do this and need a
join to reconstruct one record; kept as originally captured). Always include, by
default:

- an index column
- a timestamp (elapsed seconds is fine)
- onboard temperature
- battery state of charge
- a charging flag

...plus whatever else the specific test measures.
