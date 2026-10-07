# 24 h gapless phasor stream, calibrated instrument (started 2026-10-07)

**Firmware:** 0.10.73. **Board:** IDENTITY serial `6DA08073` (ST-Link `002800333235510737333439`, COM5).
**Logger:** `Testing/2026-10-04_contiguous_phasor_capture/phasor_stream.py` (API Topic 0x05/0x05: every
64-cycle batch, 40.69/s, all four phasors; see `docs/wp10_displacement.md`).

## Setup

* The two sensors (S1 base 200 mm, S2 150 mm) are mounted next to each other on the granite plate,
  **facing opposite directions** -- so a common tilt of the plate reads with opposite sign on the
  two, and the pair separates plate tilt from common-mode/thermal effects.
* Calibration in EEPROM (see `Testing/2026-10-07_phase_calibration/setup.md`): phase S1 -7.17 deg /
  S2 -9.19 deg, invert S1 = S2 = 1, zero S1 -6191 / S2 +1073 ppm, k S1 0.011923 / S2 0.012831.
* `auto_poweroff_s` = 0 (never powers off).
* Charging is left to the firmware, with a limit of 3 h per continuous charging stretch (charge
  inhibit); the inhibit is lifted again when the state of charge falls to 40 % so the battery cannot
  run flat (the 2026-10-04 run did: an unconditional inhibit drained it until the board reset at
  ~19 h and the stream died). If the stream stalls (e.g. a reset) the logger re-subscribes by itself
  and marks the new segment with `first_batch = 1`.
* Output `data/phasor_stream_*.csv` is git-ignored (several hundred MB); `data/*.log` has the
  console log (STATUS line every minute, charging / inhibit / stall events).

## Status

Running. Results to be added.
