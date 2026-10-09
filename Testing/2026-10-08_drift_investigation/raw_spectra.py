"""Spectral analysis of raw_captures.py output: harmonics (with their aliased apparent frequency), spurs, DC, noise floor.
python raw_spectra.py data/raw_captures_<time>.npz   -> text report + graphs/raw_spectra.png

Per state (averaged over its captures) and ADC channel (ch0 = S2, ch1 = B, ch2 = A, ch3 = S1):
  DC (mean code), rms, fundamental amplitude (uV peak AT THE ADC PIN: code x 0.1431 uV / PGA, PGA 16 on S1 / S2), harmonics 2..9 in dBc with the
  frequency they alias to (h f0 folded into 0..fs/2), the noise floor (median PSD 4..9 kHz outside the lines, nV/sqrt(Hz) at the pin) and the
  strongest non-harmonic spurs. Hann window; 6144 samples -> 3.39 Hz bins."""
import sys
import numpy as np

FS = 20833.3333
LSB_UV = 1.2 / 8388608.0 * 1e6          # uV per code at gain 1
PGA = {0: 16.0, 1: 1.0, 2: 1.0, 3: 16.0}
CH = {0: 'S2', 1: 'B', 2: 'A', 3: 'S1'}
z = np.load(sys.argv[1])
data = z['data'].astype(np.float64); names = z['name']; ns = z['n']
N = data.shape[1]
w = np.hanning(N); sw2 = np.sum(w ** 2); df = FS / N
freqs = np.fft.rfftfreq(N, 1 / FS)

def fold(f):
    return abs(f - round(f / FS) * FS)

def spectrum(x):
    X = np.fft.rfft((x - x.mean()) * w)
    return np.abs(X) ** 2                                    # |X|^2

def amp(P, f0):                                              # peak amplitude (codes) of a line near f0 (+-2 bins), Parseval
    k = int(round(f0 / df)); lo, hi = max(k - 2, 1), min(k + 3, len(P))
    return np.sqrt(4 * P[lo:hi].sum() / (N * sw2))

print(f'{sys.argv[1]}: {len(data)} captures of {N} samples ({N/FS*1000:.0f} ms), bin {df:.2f} Hz\n')
order = []
for nm in names:
    if nm not in order:
        order.append(nm)
spec_for_plot = {}
for nm in order:
    idx = np.flatnonzero(names == nm); n = int(ns[idx[0]]); f0 = FS / n
    print(f'=== {nm}  (n = {n}, f0 = {f0:.1f} Hz, {len(idx)} captures)')
    for ch in (2, 1, 3, 0):                                   # A, B, S1, S2
        x = data[idx, :, ch]
        P = np.mean([spectrum(r) for r in x], axis=0)
        scale = LSB_UV / PGA[ch]
        dc = x.mean(); rms = np.mean([r.std() for r in x])
        a1 = amp(P, f0) * scale
        line = f'  {CH[ch]:2s} ch{ch}: DC {dc:+11.1f} codes ({dc*scale:+9.1f} uV)  rms {rms*scale:8.2f} uV  fundamental {a1:10.2f} uV'
        lines_k = {int(round(f0 / df))}
        harm = []
        for h in range(2, 10):
            fh = fold(h * f0)
            if fh < 2 * df or fh > FS / 2 - 2 * df:
                continue
            ah = amp(P, fh) * scale
            lines_k.add(int(round(fh / df)))
            harm.append((h, fh, 20 * np.log10(max(ah, 1e-9) / a1) if a1 > 1 else np.nan, ah))
        # noise floor: median PSD 4..9 kHz outside +-3 bins of known lines
        psd = 2 * P / (FS * sw2) * scale ** 2                  # uV^2/Hz
        band = (freqs > 4000) & (freqs < 9000)
        mask = band.copy()
        for k in lines_k:
            mask[max(k - 3, 0):k + 4] = False
        floor_nv = np.sqrt(np.median(psd[mask])) * 1000
        print(line + f'  floor {floor_nv:7.1f} nV/rtHz')
        if a1 > 1:
            print('       harmonics: ' + '  '.join(f'h{h}->{fh:.0f}Hz {d:+.0f}dBc' for h, fh, d, ah in harm))
        # strongest spurs: peaks above 10x median PSD outside the known lines, 50..10300 Hz
        sm = (freqs > 50) & (freqs < 10300)
        cand = np.flatnonzero(sm & (psd > 30 * np.median(psd[mask])))
        spurs = []
        for k in cand:
            if any(abs(k - kk) <= 3 for kk in lines_k):
                continue
            if psd[k] >= psd[max(k - 1, 0)] and psd[k] >= psd[min(k + 1, len(psd) - 1)]:
                spurs.append((np.sqrt(psd[k] * df * 1.5) * 1.0, freqs[k]))      # rough amplitude of a line in the bin
        spurs.sort(reverse=True)
        if spurs:
            print('       spurs: ' + '  '.join(f'{f:.0f}Hz {a_:.3f}uV' for a_, f in spurs[:6]))
        if nm in ('n08_on', 'n08_off', 'n08_short_ch0_ch3') and ch in (2, 3):
            spec_for_plot[(nm, ch)] = np.sqrt(psd) * 1000
    print()

try:
    import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 1, figsize=(10.5, 8.5), sharex=True)
    for a_, ch in zip(ax, (2, 3)):
        for nm in ('n08_on', 'n08_off', 'n08_short_ch0_ch3'):
            if (nm, ch) in spec_for_plot:
                a_.semilogy(freqs, spec_for_plot[(nm, ch)], lw=.7, label=nm)
        a_.set_ylabel(f'{CH[ch]} (ch{ch}) amplitude density [nV/rtHz at the pin]'); a_.grid(alpha=.3, which='both'); a_.legend()
    ax[1].set_xlabel('frequency [Hz]'); ax[0].set_title('Raw ADC spectra, n = 8 (2604 Hz): excitation on / off / inputs shorted')
    fig.tight_layout(); fig.savefig('graphs/raw_spectra.png', dpi=105); print('wrote graphs/raw_spectra.png')
except Exception as e:
    print('plot skipped:', repr(e))
