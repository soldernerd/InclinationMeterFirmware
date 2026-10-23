"""Swap-watch analysis: 30-min means / slopes of the in-phase reading of S1, S2 before and after the cable swap (excitation-off interval).
python swap_analysis.py data/drift_swap_<time>.csv"""
import io, sys
import numpy as np

fn = sys.argv[1]
CAL = {'S1': dict(d=np.deg2rad(-7.17), k=11923e-6, inv=1), 'S2': dict(d=np.deg2rad(-9.19), k=12831e-6, inv=1)}
raw = open(fn, 'rb').read(); raw = raw[:raw.rfind(b'\n')]
hdr = raw.split(b'\n', 1)[0].decode().split(',')
num = [i for i, c in enumerate(hdr) if c not in ('step', 'mux')]
X = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=num, filling_values=np.nan)
col = {hdr[i]: j for j, i in enumerate(num)}
g = lambda n: X[:, col[n]]
t = g('t_s') / 60; exc = g('exc_on')
ok = (g('first_batch') != 1)
D = (g('iA') + 1j * g('qA')) - (g('iB') + 1j * g('qB'))
off = np.flatnonzero(exc == 0)
t_off0, t_off1 = (t[off[0]], t[off[-1]]) if len(off) else (np.nan, np.nan)
print(f'excitation off {t_off0:.1f} .. {t_off1:.1f} min of run; run length {t[-1]:.0f} min')
res = {}
for k, c in CAL.items():
    u = (g('i' + k) + 1j * g('q' + k)) / D / 16 * np.exp(-1j * c['d'])
    res[k] = -u.real / (c['k'] * 1e-3)        # um/m (sign as in other scripts)
m = ok & (exc == 1)

def bin_means(k, lo, hi, w=30):
    out = []
    for a in np.arange(lo, hi, w):
        s = m & (t >= a) & (t < a + w)
        out.append((a + w / 2, np.nanmean(res[k][s]) if s.sum() > 100 else np.nan))
    return out

def slope(k, lo, hi):
    s = m & (t >= lo) & (t < hi)
    if s.sum() < 1000:
        return np.nan
    return np.polyfit(t[s] / 60, res[k][s], 1)[0]

for k in res:
    print(f'\n{k}: 30-min means (um/m), time in min')
    print('  ' + '  '.join(f'{c:5.0f}:{v:8.1f}' for c, v in bin_means(k, 0, t[-1])))
print('\nslope um/m per hour')
print(f'{"window":28s} {"S1":>8s} {"S2":>8s}')
for name, lo, hi in [('before swap, last 2 h', t_off0 - 120, t_off0 - 2), ('before, 2nd-1st h to swap', t_off0 - 60, t_off0 - 2),
                     ('after, h1', t_off1 + 5, t_off1 + 65), ('after, h2', t_off1 + 65, t_off1 + 125), ('after, h3', t_off1 + 125, t_off1 + 185),
                     ('after, h1-3', t_off1 + 5, t_off1 + 185), ('after, last 2 h', t[-1] - 120, t[-1])]:
    print(f'{name:28s} {slope("S1", lo, hi):8.2f} {slope("S2", lo, hi):8.2f}')
for k in res:
    b = np.nanmean(res[k][m & (t > t_off0 - 30) & (t < t_off0)]); a = np.nanmean(res[k][m & (t > t_off1 + 5) & (t < t_off1 + 35)])
    print(f'{k}: 30 min mean before {b:.1f}, after {a:.1f}, step {a - b:+.1f} um/m')
