"""Analysis of a drift_tests.py frequency run (plans freq / freqalt, fw >= 0.10.91: excitation f0 = 20833.33 Hz / n).
python analyze_freq.py data/drift_freq*_<time>.csv [--skip 6]

Per contiguous run of one (step, n): the first --skip s are discarded; it prints f0, the pin amplitudes (uV peak at the ADC
input: A, B, S1, S2), and u = S/(PGA (A-B)) as in-phase / quadrature parts in um/m-EQUIVALENTS of the 2604 Hz calibration
(phase rotation, k and the sign as at n = 8; at other frequencies the sensor's sensitivity and phase differ, so these are
relative numbers, not tilt). Then, for interleaved runs (several blocks per n): the slope of the in-phase and quadrature
parts versus time for each n, and the ratio of the static values -- if the drift and the static signal share one origin the
slope ratio equals the static ratio between the frequencies."""
import io
import sys
import numpy as np

fn = sys.argv[1]
SKIP = float(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 6.0
CAL = {'S1': dict(d=np.deg2rad(-7.17), k=11923e-6), 'S2': dict(d=np.deg2rad(-9.19), k=12831e-6)}
PGA = 16.0
FS = 20833.3333
raw = open(fn, 'rb').read()
raw = raw[:raw.rfind(b'\n')]
hdr = raw.split(b'\n', 1)[0].decode().split(',')
strcols = ('step', 'mux')
num = [i for i, c in enumerate(hdr) if c not in strcols]
X = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=num, filling_values=np.nan)
col = {hdr[i]: j for j, i in enumerate(num)}
steps = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=[hdr.index('step')], dtype=str)
g = lambda n: X[:, col[n]]
t = g('t_s'); nn = g('n').astype(int); first = g('first_batch') == 1
A = g('iA') + 1j * g('qA'); B = g('iB') + 1j * g('qB'); S1 = g('iS1') + 1j * g('qS1'); S2 = g('iS2') + 1j * g('qS2'); D = A - B
um = {k: 1.0 / (c['k'] * 1e-3) for k, c in CAL.items()}
u1 = S1 / D / PGA * np.exp(-1j * CAL['S1']['d']); u2 = S2 / D / PGA * np.exp(-1j * CAL['S2']['d'])
key = np.array([f'{s}|{n}' for s, n in zip(steps, nn)])
bounds = np.concatenate([[0], np.flatnonzero(key[1:] != key[:-1]) + 1, [len(t)]])
print(f'{fn}: {len(t)} rows, {t[-1]/60:.1f} min, {len(bounds)-1} runs (first {SKIP:.0f} s of each discarded)\n')
print(f'{"step":14s} {"n":>3s} {"f0 Hz":>7s} {"dur":>5s} | {"|A|":>7s} {"|B|":>7s} {"|S1|":>7s} {"|S2|":>7s} uV | {"S1 Re":>9s} {"S1 Im":>9s} {"S2 Re":>9s} {"S2 Im":>9s}  um/m-eq | scatter S1,S2')
runs = []
for k in range(len(bounds) - 1):
    lo, hi = bounds[k], bounds[k + 1]
    m = np.zeros(len(t), bool); m[lo:hi] = True
    m &= (t >= t[lo] + SKIP) & ~first
    if m.sum() < 40:
        continue
    n = int(nn[lo]); uv = 1.2 / 8388608.0 * 1e6 / (64 * (n / 2.0) * 16384)
    r = dict(step=steps[lo], n=n, t=float(t[m].mean()) / 3600.0, dur=t[hi - 1] - t[lo],
             A=np.abs(A[m]).mean() * uv, B=np.abs(B[m]).mean() * uv, s1=np.abs(S1[m]).mean() * uv / PGA, s2=np.abs(S2[m]).mean() * uv / PGA,
             r1=-u1[m].real.mean() * um['S1'], i1=-u1[m].imag.mean() * um['S1'], r2=-u2[m].real.mean() * um['S2'], i2=-u2[m].imag.mean() * um['S2'],
             sd1=np.std(u1[m].real) * um['S1'], sd2=np.std(u2[m].real) * um['S2'])
    runs.append(r)
    print(f'{r["step"]:14s} {n:3d} {FS/n:7.1f} {r["dur"]:5.0f} | {r["A"]:7.0f} {r["B"]:7.0f} {r["s1"]:7.1f} {r["s2"]:7.1f}    | '
          f'{r["r1"]:+9.1f} {r["i1"]:+9.1f} {r["r2"]:+9.1f} {r["i2"]:+9.1f}          | {r["sd1"]:.2f} {r["sd2"]:.2f}')

by_n = {}
for r in runs:
    by_n.setdefault(r['n'], []).append(r)
multi = {n: v for n, v in by_n.items() if len(v) >= 4}
if len(multi) >= 2:
    print('\nInterleaved blocks: slope of the block means versus time [um/m-eq per hour], and mean static values')
    stat = {}
    for n, v in sorted(multi.items()):
        T = np.array([r['t'] for r in v])
        out = [f'n={n:2d} (f0 {FS/n:6.1f} Hz, {len(v)} blocks)']
        for lab, key_ in (('S1 Re', 'r1'), ('S1 Im', 'i1'), ('S2 Re', 'r2'), ('S2 Im', 'i2')):
            y = np.array([r[key_] for r in v]); sl = np.polyfit(T, y, 1)[0]
            out.append(f'{lab} slope {sl:+7.2f} (mean {y.mean():+8.1f})'); stat[(n, lab)] = (sl, y.mean())
        print('  ' + ' | '.join(out))
    ns = sorted(multi)
    n0 = 8 if 8 in multi else ns[0]
    for n in ns:
        if n == n0:
            continue
        for lab in ('S1 Re', 'S2 Re'):
            s0, m0 = stat[(n0, lab)]; s1, m1 = stat[(n, lab)]
            print(f'  {lab}: n={n} vs n={n0}:  slope ratio {s1/s0 if s0 else float("nan"):+6.2f}   static-value ratio {m1/m0 if m0 else float("nan"):+6.2f}')
