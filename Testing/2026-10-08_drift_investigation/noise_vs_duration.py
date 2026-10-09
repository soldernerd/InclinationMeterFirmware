"""Is the n = 16 noise advantage just the longer batch? Equal-duration comparison from an interleaved freqalt run.
python noise_vs_duration.py data/drift_freqalt16_<time>.csv [--skip 8]

n = 8: one batch = 64 cycles x 8 samples = 512 samples = 24.6 ms.  n = 16: one batch = 64 x 16 = 1024 samples = 49.2 ms.
Equal duration/samples: n=16 single batch (49.2 ms) vs n=8 average of 2 consecutive batches (49.2 ms); and 98 ms: n=16 pairs vs n=8 quadruples.
Noise per block = std of the successive differences / sqrt(2) of the (averaged) in-phase and quadrature u, in um/m-equivalents of the
2604 Hz calibration (the sensor's k at 1302 Hz is unknown, so the n=16 numbers are in raw u units). 'noise x sqrt(T)' normalises for duration
(white noise: constant)."""
import io, sys
import numpy as np

fn = sys.argv[1]
SKIP = float(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 8.0
CAL = {'S1': dict(d=np.deg2rad(-7.17), k=11923e-6), 'S2': dict(d=np.deg2rad(-9.19), k=12831e-6)}
raw = open(fn, 'rb').read(); raw = raw[:raw.rfind(b'\n')]
hdr = raw.split(b'\n', 1)[0].decode().split(',')
num = [i for i, c in enumerate(hdr) if c not in ('step', 'mux')]
X = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=num, filling_values=np.nan)
col = {hdr[i]: j for j, i in enumerate(num)}
steps = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=[hdr.index('step')], dtype=str)
g = lambda n: X[:, col[n]]
t = g('t_s'); nn = g('n').astype(int); first = g('first_batch') == 1
D = (g('iA') + 1j * g('qA')) - (g('iB') + 1j * g('qB'))
u = {k: (g('i' + k) + 1j * g('q' + k)) / D / 16 * np.exp(-1j * c['d']) for k, c in CAL.items()}
um = {k: 1.0 / (c['k'] * 1e-3) for k, c in CAL.items()}
FS = 20833.3333
key = np.array([f'{s}|{n}' for s, n in zip(steps, nn)])
bounds = np.concatenate([[0], np.flatnonzero(key[1:] != key[:-1]) + 1, [len(t)]])

def noise(z, m):
    """sigma of block-averaged (m batches) values from successive differences, in the units of z."""
    k = (len(z) // m) * m
    if k < 6 * m:
        return np.nan
    a = z[:k].reshape(-1, m).mean(axis=1)
    return np.std(np.diff(a)) / np.sqrt(2)

rows = {8: [], 16: []}
for b in range(len(bounds) - 1):
    lo, hi = bounds[b], bounds[b + 1]
    n = int(nn[lo])
    if n not in rows:
        continue
    m = np.zeros(len(t), bool); m[lo:hi] = True
    m &= (t >= t[lo] + SKIP) & ~first
    if m.sum() < 400:
        continue
    T1 = 64 * n / FS                                         # one batch in seconds
    r = {}
    for k in ('S1', 'S2'):
        zr = -u[k][m].real * um[k]; zi = -u[k][m].imag * um[k]
        for mult in (1, 2, 4):
            r[(k, mult)] = (noise(zr, mult), noise(zi, mult), T1 * mult)
    rows[n].append(r)

def med(n, k, mult, idx):
    v = [r[(k, mult)][idx] for r in rows[n] if not np.isnan(r[(k, mult)][idx])]
    return np.median(v), len(v)

print(f'{fn}\nblocks: n=8 {len(rows[8])}, n=16 {len(rows[16])}; block medians of the noise (um/m-eq, 1 sigma), in-phase part [and quadrature]\n')
print(f'{"sensor":6s} {"duration":>9s} | {"n=8 averaged":>22s} | {"n=16 averaged":>22s} | ratio n16/n8')
for k in ('S1', 'S2'):
    for (m8, m16) in ((1, None), (2, 1), (4, 2), (None, 4)):
        d8 = f'{med(8,k,m8,0)[0]:8.3f} [{med(8,k,m8,1)[0]:6.3f}]' if m8 else '-'
        d16 = f'{med(16,k,m16,0)[0]:8.3f} [{med(16,k,m16,1)[0]:6.3f}]' if m16 else '-'
        dur = (64 * 8 / FS * m8) if m8 else (64 * 16 / FS * m16)
        ratio = f'{med(16,k,m16,0)[0] / med(8,k,m8,0)[0]:5.2f}' if (m8 and m16) else ''
        print(f'{k:6s} {dur*1000:7.1f} ms | {d8:>22s} | {d16:>22s} | {ratio}')
print('\nnormalised: noise x sqrt(duration)  [um/m-eq x sqrt(s)], in-phase part')
for k in ('S1', 'S2'):
    out = []
    for n in (8, 16):
        for mult in (1, 2, 4):
            s, _ = med(n, k, mult, 0); T = 64 * n / FS * mult
            out.append(f'n={n} x{mult} ({T*1000:5.1f} ms): {s*np.sqrt(T):.4f}')
    print(f'  {k}: ' + ' | '.join(out))
