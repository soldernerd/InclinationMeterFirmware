# Test data

Everything the host tests need is in this folder, so a plain clone can run `make` in `tests/`.

| File | What |
|---|---|
| `phasor_excerpt.bin` | 26 000 consecutive batches (10.6 min, 884 KB) of real phasors from the 19 h gapless recording of 2026-10-04/05 (`Testing/2026-10-04_contiguous_phasor_capture`), starting at batch 216 000 (1.47 h in, working hours). Chosen because it contains both quiet stretches and about 14 % elevated Im(x) step power. |
| `golden_displacement.csv` | What `Services/svc_displacement.c` produced on that excerpt at fw 0.10.73, *before* its maths was extracted into `Math/math_displacement.c` and `Math/math_quality.c`. The replay in `test_golden_displacement.c` must reproduce it. |

## Excerpt format

34-byte little-endian records, no header: `iB qB iA qA iS1 qS1 iS2 qS2` as `float32`, then the batch's `uint16` cycle
counter (`seq`; consecutive batches differ by 64 modulo 65536). `count = file size / 34`. The floats are the
phasors exactly as the firmware's phasor stream (API Topic 0x05 / resource 0x05) sends them; the replay converts
them back to the int64 sums (they are exact integers) before demodulating.

## Golden file

`kind,batch,a,b,c,d,e,f`, in time order, `%.9g` floats:

* `raw`  every 50th batch: delta1, delta2, residual1, residual2, ok
* `disp` every published display value: out1, out2, doubtful1, doubtful2, valid, seq
* `prec` every finished precision measurement (one started every 1500 batches, 5 s time budget): delta1, delta2,
  differential, failed, batches since the trigger
* `sum`  totals: batches, degenerate batches, amplitude faults, last display seq, windows pushed

Settings used: k = 0.0213 per mm/m for both sensors, PGA 16, zero +1234 / -4321 ppm, phase -6.00 / -9.15 degrees,
S2 inverted (only affects the reported precision result). Two disturbances are injected by the test: batch 9000 is
dropped and batch 15000 has A == B. Values are compared with a relative tolerance of 1e-5 (another compiler or FMA
contraction may differ in the last bits); flags, counts and cadence must match exactly.

## Regenerating the golden after an intended behaviour change

The golden pins the *behaviour*, so it only changes when the behaviour is meant to change (a new filter length, a
different threshold, ...). Replace the expected values with the new, reviewed output and say why in the commit. The
test prints which batches differ. There is no generator checked in: the file came from the firmware's own service code
driven on the host; to produce a new one, drive the changed pipeline the way `replay_reproduces_the_golden_reference`
does and write the same `kind,...` lines.

## The full recording

The 19 h recording is 534 MB as CSV and does not belong in git. It was converted losslessly (float32 phasors are
exact in the CSV) to `phasor_stream_19h_float32.npz` (80 MB, numpy `savez_compressed`: `phasors` (N,8), `seq`,
`t_s`, temperature, SoC, charging, ...) next to the CSV in `Testing/2026-10-04_contiguous_phasor_capture/data/`
(gitignored). SHA-256 of the npz: `084c6388bc52be0ffe53f45b0f86b1fa1e4dd9b1bf4950f9a808af469e92b869`.
