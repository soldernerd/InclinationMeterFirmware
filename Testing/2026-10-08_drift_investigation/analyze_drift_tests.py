"""Analysis of a drift_tests.py CSV: per-state phasor statistics and the excitation-proportional / additive split.
python analyze_drift_tests.py data/drift_<plan>_<time>.csv [--skip 3]

For every contiguous run of one applied state (step name, exc, phase, mux) it discards the first --skip seconds and prints
mean raw amplitudes in uV peak at the ADC input pins (A, B, S1, S2), the ratio u = S/(PGA*(A-B)) as in-phase / quadrature
parts in um/m at the current calibration, and its scatter. Then:
 * phase reversal: u(phase) for phase 0 / 90 / 180 / 270 deg. Excitation-proportional signals (tilt, sensor, leakage) give
   the same u at 0 and 180; an additive signal independent of the excitation flips sign: c = (u(0) - u(180)) / 2.
 * shorted channels / excitation off: raw S (uV) = the additive terms alone.
 * chopping (if present): the chopped reading (u(0) + u(180)) / 2 per pair and its additive part (u(0) - u(180)) / 2 per pair,
   hourly means -- does the S1 ramp live in the additive or in the proportional part?
"""
import io
import sys
import numpy as np

fn = sys.argv[1]
SKIP = float(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 3.0
CAL = {'S1': dict(d=np.deg2rad(-7.17), zero=-6191e-6, k=11923e-6), 'S2': dict(d=np.deg2rad(-9.19), zero=1073e-6, k=12831e-6)}
PGA = 16.0
UV = 1.2 / 8388608.0 * 1e6 / (64 * 4 * 16384)          # raw batch sum -> uV peak at the ADC pin for PGA 1 (divide by PGA for S)
with open(fn, 'rb') as fh:
    raw = fh.read()
raw = raw[:raw.rfind(b'\n')]
hdr = raw.split(b'\n', 1)[0].decode().split(',')
num = [i for i, n in enumerate(hdr) if n not in ('step', 'mux')]
X = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=num, filling_values=np.nan)
cols = {hdr[i]: j for j, i in enumerate(num)}
steps = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=[hdr.index('step')], dtype=str)
muxs = np.genfromtxt(io.BytesIO(raw), delimiter=',', skip_header=1, usecols=[hdr.index('mux')], dtype=str)
g = lambda n: X[:, cols[n]]
t = g('t_s'); first = g('first_batch') == 1
A = g('iA') + 1j * g('qA'); B = g('iB') + 1j * g('qB'); S1 = g('iS1') + 1j * g('qS1'); S2 = g('iS2') + 1j * g('qS2')
D = A - B
exc = g('exc_on'); phase = g('phase')
state = np.array([f'{s}|{int(e)}|{int(p)}|{m}' for s, e, p, m in zip(steps, exc, phase, muxs)])
change = np.flatnonzero(state[1:] != state[:-1]) + 1
bounds = np.concatenate([[0], change, [len(t)]])
print(f'{fn}: {len(t)} rows, {t[-1]/60:.1f} min, {len(bounds)-1} state runs (first {SKIP:.0f} s of each discarded)\n')
um = {n: 1.0 / (CAL[n]['k'] * 1e-3) for n in CAL}                    # u -> um/m  (reading = -(Re u - zero)/k)
print(f'{"run":4s} {"step":20s} {"exc":>3s} {"phase":>5s} {"mux":>5s} {"dur":>6s} | {"|A|":>7s} {"|B|":>7s} {"|S1|":>7s} {"|S2|":>7s} uV | '
      f'{"S1 Re":>9s} {"S1 Im":>8s} {"S2 Re":>9s} {"S2 Im":>8s} um/m (rot., zero not subtracted)  scatter(S1,S2)')
runs = []
for k in range(len(bounds) - 1):
    lo, hi = bounds[k], bounds[k + 1]
    m = np.zeros(len(t), bool); m[lo:hi] = True
    m &= (t >= t[lo] + SKIP) & ~first
    if m.sum() < 40:
        continue
    st = state[lo].split('|')
    u1 = S1[m] / D[m] / PGA * np.exp(-1j * CAL['S1']['d']); u2 = S2[m] / D[m] / PGA * np.exp(-1j * CAL['S2']['d'])
    row = dict(k=k, step=st[0], exc=int(st[1]), phase=int(st[2]), mux=st[3], dur=t[hi - 1] - t[lo], m=m,
               A=np.abs(A[m]).mean() * UV, B=np.abs(B[m]).mean() * UV, S1=np.abs(S1[m]).mean() * UV / PGA, S2=np.abs(S2[m]).mean() * UV / PGA,
               u1=u1.mean(), u2=u2.mean(), s1=np.std(u1.real) * um['S1'], s2=np.std(u2.real) * um['S2'],
               rawS1=S1[m].mean() * UV / PGA, rawS2=S2[m].mean() * UV / PGA, Dm=D[m].mean())
    runs.append(row)
    print(f'{k:4d} {st[0]:20s} {st[1]:>3s} {st[2]:>5s} {st[3]:>5s} {row["dur"]:6.0f} | {row["A"]:7.0f} {row["B"]:7.0f} {row["S1"]:7.1f} {row["S2"]:7.1f}    | '
          f'{-row["u1"].real*um["S1"]:+9.1f} {-row["u1"].imag*um["S1"]:+8.1f} {-row["u2"].real*um["S2"]:+9.1f} {-row["u2"].imag*um["S2"]:+8.1f}   {row["s1"]:.2f} {row["s2"]:.2f}')

# ---- phase reversal
print('\nPhase reversal (normal mux, excitation on): u = S/(PGA D) per phase, mean over runs [um/m scale, in-phase sign as in the reading]')
byph = {}
for r in runs:
    if r['exc'] == 1 and r['mux'] == '0000' and not r['step'].startswith('chop'):
        byph.setdefault(r['phase'], []).append(r)
if 0 in byph and 2048 in byph:
    for n, key in (('S1', 'u1'), ('S2', 'u2')):
        u0 = np.mean([r[key] for r in byph[0]]); u180 = np.mean([r[key] for r in byph[2048]])
        c = (u0 - u180) / 2; p = (u0 + u180) / 2
        print(f'  {n}: u(0) {-u0.real*um[n]:+9.2f} {-u0.imag*um[n]:+8.2f}j   u(180) {-u180.real*um[n]:+9.2f} {-u180.imag*um[n]:+8.2f}j   '
              f'proportional part (u0+u180)/2 = {-p.real*um[n]:+9.2f} {-p.imag*um[n]:+8.2f}j   ADDITIVE part (u0-u180)/2 = {-c.real*um[n]:+8.2f} {-c.imag*um[n]:+8.2f}j um/m')
    for ph in (1024, 3072):
        if ph in byph:
            for n, key in (('S1', 'u1'), ('S2', 'u2')):
                uu = np.mean([r[key] for r in byph[ph]])
                print(f'  {n} u({ph*360//4096:3d} deg) = {-uu.real*um[n]:+9.2f} {-uu.imag*um[n]:+8.2f}j')
else:
    print('  (no phase 0 / 180 runs in this file)')

# ---- shorted / off
print('\nAdditive terms alone (raw S in uV peak at the input pin, mean complex value; compare 1 um/m ~ 15 uV):')
for r in runs:
    if r['mux'] != '0000' or r['exc'] == 0:
        print(f'  {r["step"]:20s} exc={r["exc"]} mux={r["mux"]}  S1 {abs(r["rawS1"]):7.2f} uV  S2 {abs(r["rawS2"]):7.2f} uV   |D| {abs(r["Dm"])*UV:9.1f} uV')

# ---- chopped pairs
chop = np.array([s.startswith('chop') for s in steps])
if chop.any():
    print('\nChopped run: pairs of consecutive 0/180 deg runs; hourly means of the proportional part (u0+u180)/2 and the additive part (u0-u180)/2 [um/m, in-phase]:')
    cr = [r for r in runs if r['step'].startswith('chop')]
    tm = np.array([t[np.flatnonzero(r['m'])[0]] for r in cr]) / 3600.0
    for n, key in (('S1', 'u1'), ('S2', 'u2')):
        P, Dd, T = [], [], []
        for i in range(len(cr) - 1):
            if cr[i]['phase'] == 0 and cr[i + 1]['phase'] == 2048:
                P.append((cr[i][key] + cr[i + 1][key]) / 2); Dd.append((cr[i][key] - cr[i + 1][key]) / 2); T.append(tm[i])
        P = -np.array(P).real * um[n]; Dd = -np.array(Dd).real * um[n]; T = np.array(T)
        if len(T) > 4:
            sp = np.polyfit(T, P, 1)[0]; sd = np.polyfit(T, Dd, 1)[0]
            print(f'  {n}: {len(T)} pairs over {T.max()-T.min():.1f} h:  proportional part slope {sp:+.2f} um/m per h,   additive part slope {sd:+.2f} um/m per h  '
                  f'(additive range {Dd.max()-Dd.min():.1f} um/m)')
