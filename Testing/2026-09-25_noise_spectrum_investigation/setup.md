# Noise spectrum investigation

**Date:** 2026-09-25. **Firmware:** 0.10.35. **Board:** #2.

## Purpose

Early noise-characterization pass on the raw AFE signal chain, before the
per-channel-jump investigation in [[2026-09-26_10min_streaming_monitor]] existed:
is the noise floor narrowband (mains hum, digital-switching coupling into the
analog front end) or broadband (consistent with ordinary analog/thermal noise)?

## Method

Bulk raw-ADC captures (`PythonTestCode/bulk_adc_csv.py`'s `capture()`, reused by
`PythonTestCode/adc_signal_analysis.py`) of all 4 channels at the ADC's native
~20833 Hz, FFT'd with a Hann window. Three captures compared:

- **A: baseline** — all subsystems running normally (display, BLE, LEDs on).
- **C: digital-subsystems-off** — display, BLE and LEDs disabled for the capture,
  to see whether any spectral energy present in A disappears in C (which would
  point at digital-switching noise coupling into the analog front end).
- A general **4-channel signal-quality** capture (waveforms + spectrum) on ch0-ch3
  to confirm all channels show a clean sine at the expected ~2604 Hz fundamental
  with no gross anomalies.

Note: the specific comparison script used for A vs C was run ad hoc (not saved
as a standalone reusable script — `adc_signal_analysis.py` is the closest
reusable equivalent for a single-capture spectral report); no raw CSV was kept
for this particular test, only the resulting plots. If this comparison needs
re-running, `adc_signal_analysis.py --port COM6` reproduces the single-capture
FFT/THD/SNR numbers; the A/B on/off toggle would need to be scripted around it.

## Files

- `graphs/board2_signal_quality.gif` — all 4 channels, time-domain waveform +
  spectrum, one representative capture.
- `graphs/ch3_noise_spectrum_comparison.gif` — ch3 (S1) full spectrum (0-10.4kHz)
  and low-frequency zoom (0-500Hz), baseline vs digital-subsystems-off overlaid.
- `graphs/ch3_lowfreq_zoom_fixed.gif` — ch3 low-frequency zoom alone, with dashed
  reference lines at 50/60/100/120/150/180 Hz (mains hum and its harmonics,
  both 50Hz- and 60Hz-region in case of ground-loop/USB-supply coupling).
