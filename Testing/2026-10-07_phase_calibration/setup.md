# Phase / zero / k calibration of the instrument (2026-10-07)

**Firmware:** 0.10.73. **Board:** IDENTITY serial `6DA08073` (ST-Link `002800333235510737333439`, COM5).
Instrument on a granite plate; tilt steps are made with 0.1 mm shims under the supports.

## Mechanical data

| Sensor | Base length L (distance between supports) |
|---|---|
| S1 | **200 mm** |
| S2 | **150 mm** |

A shim of thickness h under one support tilts sensor i by h / L_i. Front-vs-back (0.1 mm moved from
front to back = 0.2 mm step): S1 1.000 mm/m, S2 1.333 mm/m. Flat-vs-front (0.1 mm): S1 0.500 mm/m,
S2 0.667 mm/m. The firmware reading is in mm/m (`(r - zero)/k`), so `k_new = k_old x measured / true`.

## Method

`Testing/2026-10-04_contiguous_phasor_capture/phasor_stream.py` logs every 64-cycle batch (gapless,
40.69/s) for 90 s per tilt state. Per state the mean of `u = S/D` (D = A - B) is formed; only
differences between states are used, so zero offsets cancel.

## Steps and results

1. **Phase** (front vs back shim): the angle of the change of `u` is 172.83 deg (S1) / 170.81 deg (S2),
   i.e. delta = -7.17 / -9.19 deg mod 180 plus a sign inversion. Written: S1 -717 cdeg, S2 -919 cdeg
   (Calibrations 0x0D/0x0E) and invert = 1 for both (0x0B/0x0C) so that front-up reads positive;
   user confirmed the signs. After the rotation the quadrature part of `u` is identical in the front,
   flat and back states (0.0428 / 0.0427 / 0.0428 for S1), i.e. the phase constants are good.
2. **Zero**: flip calibration run by the user on the instrument: S1 -6191 ppm, S2 +1073 ppm.
3. **k**: written 2026-10-07 from the same-seating steps (flat -> front 0.1 mm, front -> back 0.2 mm), mean of the two:
   S1 k = 0.011923 (k_micro 11923; steps 0.01163 / 0.01221), S2 k = 0.012831 (k_micro 12831; steps 0.01293 / 0.01273).
   Verified after a reset. Check: with the 0.1 mm shims at the back the display read S1 -0.512 / S2 -0.624 mm/m
   against the true -0.500 / -0.667. Uncertainty: the shim step (+-5 % tolerance, seating); an earlier seating
   gave S1 0.0138-0.0145 and S2 0.0131-0.0133, so S1 in particular could be 10-15 % off. Zero (ppm of r) does not
   depend on k and stays valid. `analyze_k.py` reproduces the numbers from the capture files.

## Data

`data/phase_cal_shim_on_1.csv` (front shim), `phase_cal_shim_back_1.csv` (back shim),
`phase_cal_flat_1.csv` (no shim), `phase_cal_front_2.csv` / `phase_cal_back_2.csv` (second seating, after the flip calibration; the k estimate uses flat_1, front_2, back_2). Captured before / after the flip calibration; the instrument was
re-seated in between, so states from different seatings are only comparable as a rough check.
