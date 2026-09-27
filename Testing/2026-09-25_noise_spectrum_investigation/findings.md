# Findings

**All 4 channels show a clean sine** at the ~2604 Hz excitation fundamental
(`board2_signal_quality.gif`) with harmonics ~50-70dB down from the
fundamental — no gross distortion or channel-specific anomaly at a glance.

**Digital-subsystems on vs off made no visible difference to the noise floor**
(`ch3_noise_spectrum_comparison.gif`, top panel): the broadband floor sits at
roughly -100 to -120dB in BOTH captures, overlapping within run-to-run
variation across the whole 0-10.4kHz span. If display/BLE/LED switching were
coupling meaningfully into the analog front end, disabling them should have
visibly lowered the floor at some frequency — it didn't.

**No mains-hum or narrowband spurs** (`ch3_lowfreq_zoom_fixed.gif`): no peaks
at 50/60/100/120/150/180 Hz (checked explicitly, marked with reference lines)
above the same broadband noise floor.

**Conclusion:** the AFE noise floor is broadband, not narrowband — consistent
with ordinary analog/thermal noise in the signal path (op-amps, PGA, ADC
input-referred noise) rather than digital switching noise or mains coupling.
This ruled out two easy/attractive explanations early, before the per-channel
micro-jump noise was later traced (via [[2026-09-26_sensor_swap_test]]) to one
specific physical sensor head rather than anything in the digital or power
domain.
