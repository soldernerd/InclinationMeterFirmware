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

## Data policy

* **Up to 5 MB per file: commit the raw data** (`data/*.csv`), unprocessed, next to the script that recorded it.
  (Two older tests are above that and stay tracked as they were: `2026-09-27_2hr_charging_drift_test`, 13 MB, and
  `2026-09-29_pga1_drift_comparison`, 29 MB.)
* **Above 5 MB: do not commit.** Git history is forever and every clone downloads it; GitHub refuses single files
  over 100 MB. Put such a file under `data/large/` (ignored by `.gitignore` as `Testing/*/data/large/`), or leave it where
  it is if it is already excluded. Then run `python Testing/large_files.py --write Testing/<test>` -- it writes
  `LARGE_FILES.md` in the test folder with each file's size, SHA-256 and a one-line description, so a copy handed to
  someone else can be checked against the original. Commit that manifest, `setup.md`, `findings.md`, the scripts and the
  graphs.
* **If the data is needed by an automated test,** commit a small excerpt instead and say where it came from. Example:
  `tests/data/phasor_excerpt.bin` (884 KB) is a slice of the 19 h recording of `2026-10-04_contiguous_phasor_capture`.
* Prefer a compact lossless format for big recordings (float32 arrays in a compressed `.npz`: 80 MB instead of 534 MB of
  CSV for the 19 h stream) and keep the converter script in the test folder.
* `python Testing/large_files.py` (no arguments) lists every large local-only file and exits non-zero if one has no manifest.
* Archiving the originals (a Release asset, a shared drive) is the owner's decision; the manifest says which file to look for.

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
| `2026-09-27_board1_vbat_calibration` | Board 1 (grey) battery-ADC calibration, same method/session | 0.10.53 | Same ballpark result as board 2 (ratio within 0.13%, similar offset) — sub-1.3mV accurate 3.6-4.2V |
| `2026-09-27_24hr_granite_plate_test` | 24h unattended monitor on the granite plate: raw displacement + phasors + full error-term set | 0.10.53 | Granite quieter than office desk but not big-jump-free; noise drops ~order of magnitude when operator leaves (common-mode); natural charge event causes a large common-mode jump + 6-10x noise, diff mostly immune; board-side (PGA) implicated for common-mode temp drift, sensor-side implicated for the uncancelled diff drift |
| `2026-09-29_pga1_drift_comparison` | 1h drift comparison at PGA=1 (vs the 24h test's PGA=16), same method | 0.10.58 | No dramatic difference observed vs PGA=16 quiet periods, but no thermal transient occurred during this run — inconclusive on the original PGA-tempco question, see `findings.md` |
| `2026-09-30_bulk_adc_30s_interval` | 24h monitor via periodic (every 30s) full-rate raw-ADC bulk captures instead of a continuous stream | 0.10.62 | Complete (2880 captures, no gaps). First cycle of each capture is stale in ~10 %; sensor noise is dominated by a ~20 Hz pendulum resonance (10 dB louder by day) that leaks through the boxcar averaging; an apodised window cuts daytime scatter ~2x; dropping flagged batches makes it worse. See `findings.md`. Raw `*.bin` not committed |
| `2026-10-07_phase_calibration` | Phase / sign / zero / k calibration of the two sensors with 0.1 mm shim steps (front / flat / back) | 0.10.73 | Phase S1 -7.17 / S2 -9.19 deg with invert = 1 (front-up reads positive), quadrature part constant across tilt states; k S1 0.011923, S2 0.012831 from the same-seating steps (base lengths 200 / 150 mm); S1 +-10-15 % uncertain across seatings. See `setup.md` |
| `2026-10-07_24h_phasor_stream_calibrated` | 24 h gapless 40.69/s phasor stream (API Topic 0x05/0x05) on the calibrated pair, mounted facing opposite directions | 0.10.73 | In progress. CSV git-ignored. Logger re-subscribes after a stall and lifts the 3 h charge inhibit at 40 % SoC (the previous 19 h stream died when an unconditional inhibit drained the battery). See `setup.md` |

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
