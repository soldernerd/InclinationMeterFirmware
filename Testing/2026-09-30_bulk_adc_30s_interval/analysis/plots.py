import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from scipy import signal, stats
import load, stats_core as sc

plt.rcParams.update({'figure.dpi': 110, 'axes.grid': True, 'grid.alpha': .3, 'font.size': 9})
os.makedirs('graphs', exist_ok=True)
G = 'graphs/'
t, Pfull, dc = load.get()
P = Pfull[:, 1:]
A = np.abs(P)
Af = np.abs(Pfull)
d = np.load('stats.npz')
th = t / 3600
FS = 20833.333 / 8
col = ['tab:green', 'tab:blue', 'tab:red', 'tab:purple']
lab = ['ch0 (S2)', 'ch1 (B)', 'ch2 (A)', 'ch3 (S1)']


def wrap_abs(ch):
    """Absolute capture phase (deg) about run median; removes the 45-deg start-index ambiguity."""
    r = d[f'{ch}_refphase_deg']
    w = ((r + 22.5) % 45) - 22.5
    return ((w - np.median(w) + 22.5) % 45) - 22.5


# 1 ---- cycle-0 artifact
rest = Af[:, 1:]
mr = np.median(rest, 1, keepdims=True)
zf = (Af - mr) / (1.4826 * np.median(np.abs(rest - mr), 1, keepdims=True))
fig, ax = plt.subplots(1, 3, figsize=(15, 4))
for c in (1, 2):
    ax[0].bar(np.arange(16) + (c - 1.5) * .35, (np.abs(zf[:, :16, c]) > 10).mean(0) * 100, .35, label=lab[c], color=col[c])
ax[0].set_xlabel('cycle index within capture'); ax[0].set_ylabel('% of captures with |z|>10')
ax[0].set_title('Outliers live only in cycle 0'); ax[0].legend()
bad = np.abs(zf[:, 0, 1]) > 10
rel = 1e6 * (Af[:, 0, 1] / np.median(Af[:, 1:, 1], 1) - 1)
ax[1].plot(th[~bad], rel[~bad], '.', ms=2, color='gray', label='clean cycle 0')
ax[1].plot(th[bad], rel[bad], '.', ms=3, color='tab:red', label=f'stale first sample ({bad.sum()} caps, {bad.mean()*100:.1f}%)')
ax[1].set_xlabel('time (h)'); ax[1].set_ylabel('cycle-0 amplitude error, ch1 (ppm)')
ax[1].set_title('Cycle-0 error vs time (random, ~10%)'); ax[1].legend(markerscale=4)
ax[2].plot(th, Af.std(1)[:, 1], lw=.6, color='tab:red', label='with cycle 0')
ax[2].plot(th, A.std(1)[:, 1], lw=.6, color='k', label='cycle 0 dropped')
ax[2].set_yscale('log'); ax[2].set_xlabel('time (h)'); ax[2].set_ylabel('within-capture amp std, ch1 (counts)')
ax[2].legend(); ax[2].set_title('One bad cycle inflates std ~5x')
plt.tight_layout(); plt.savefig(G + '1_cycle0_artifact.png'); plt.close()
np.save('cycle0_bad_mask.npy', bad)

# 2 ---- location time series
fig, ax = plt.subplots(4, 2, figsize=(15, 11), sharex=True)
for c, ch in enumerate(sc.CH):
    m = np.median(d[f'{ch}_amp_mean'])
    ax[c, 0].plot(th, 1e6 * (d[f'{ch}_amp_mean'] / m - 1), lw=.5, color=col[c], label='mean')
    ax[c, 0].plot(th, 1e6 * (d[f'{ch}_amp_median'] / m - 1), lw=.5, color='k', alpha=.5, label='median')
    ax[c, 0].set_ylabel(f'{ch} amp (ppm)'); ax[c, 0].legend(loc='upper right')
    ax[c, 1].plot(th, wrap_abs(ch) * 1000, lw=.5, color=col[c]); ax[c, 1].set_ylabel(f'{ch} phase (mdeg)')
ax[0, 0].set_title('Per-capture amplitude, relative to run median (cycle 0 dropped)')
ax[0, 1].set_title('Per-capture phase (median phasor), start ambiguity removed')
ax[3, 0].set_xlabel('time (h)'); ax[3, 1].set_xlabel('time (h)')
plt.tight_layout(); plt.savefig(G + '2_location_timeseries.png'); plt.close()

# 3 ---- moments time series
fig, ax = plt.subplots(3, 2, figsize=(15, 9), sharex=True)
for j, (q, u) in enumerate((('amp', 'counts'), ('ph', 'mdeg'))):
    for c, ch in enumerate(sc.CH):
        for i, s in enumerate(('std', 'skew', 'exkurt')):
            ax[i, j].plot(th, d[f'{ch}_{q}_{s}'], lw=.5, color=col[c], label=ch)
    ax[0, j].set_yscale('log'); ax[0, j].set_ylabel(f'std ({u})'); ax[1, j].set_ylabel('skew')
    ax[2, j].set_ylabel('excess kurtosis'); ax[2, j].set_yscale('symlog', linthresh=3)
    ax[0, j].set_title({'amp': 'Amplitude |phasor|', 'ph': 'Phase (about capture median)'}[q])
    ax[2, j].set_xlabel('time (h)')
ax[0, 0].legend(ncol=4)
plt.tight_layout(); plt.savefig(G + '3_moments_timeseries.png'); plt.close()

# 4 ---- pooled residual histograms
fig, ax = plt.subplots(2, 4, figsize=(16, 6.5))
dph, _ = sc.phase_residual_mdeg(P)
for r, (X, nm) in enumerate(((A, 'amp'), (dph, 'phase'))):
    for c in range(4):
        x = X[:, :, c]
        z = ((x - np.median(x, 1, keepdims=True)) / sc.mad_sigma(x)[:, None]).ravel()
        h, e = np.histogram(z, bins=np.linspace(-8, 12, 161), density=True)
        xc = (e[1:] + e[:-1]) / 2
        ax[r, c].semilogy(xc, np.where(h > 0, h, np.nan), color=col[c], lw=1.2, label='data (per-capture MAD-z)')
        ax[r, c].semilogy(xc, stats.norm.pdf(xc), 'k--', lw=.8, label='N(0,1)')
        ax[r, c].set_ylim(1e-6, 1)
        ax[r, c].set_title(f'{lab[c]} {nm}: P(|z|>4)={np.mean(np.abs(z) > 4):.1e}')
        ax[r, c].set_xlabel('robust z')
ax[0, 0].legend(fontsize=7)
plt.tight_layout(); plt.savefig(G + '4_residual_histograms.png'); plt.close()

# 5 ---- PSD within capture
fig, ax = plt.subplots(1, 2, figsize=(14, 4.5))
for q, X, a_ in (('amplitude', A, ax[0]), ('phase', dph, ax[1])):
    for c in range(4):
        x = X[:, :, c] - X[:, :, c].mean(1, keepdims=True)
        f, p = signal.welch(x, fs=FS, nperseg=767, window='hann', axis=1, detrend=False)
        a_.loglog(f[1:], np.sqrt(p.mean(0)[1:]), color=col[c], label=lab[c])
    for h_ in (50, 100, 150):
        a_.axvline(h_, color='gray', ls=':', lw=.7)
    a_.set_xlabel('frequency of phasor sequence (Hz)')
    a_.set_ylabel('ASD (counts/sqrt(Hz))' if q == 'amplitude' else 'ASD (mdeg/sqrt(Hz))')
    a_.set_title(f'{q}: mean spectrum of 2880 captures (dotted: 50/100/150 Hz)'); a_.legend()
plt.tight_layout(); plt.savefig(G + '5_psd.png'); plt.close()


# 6 ---- estimator comparison (30-s scatter relative to plain mean)
def st(x):
    return np.std(np.diff(x)) / np.sqrt(2)


def est(ch, q, e):
    return d[f'{ch}_amp_{e}'] if q == 'amp' else wrap_abs(ch) + d[f'{ch}_ph_{e}'] / 1000


ests = ['mean', 'median', 'trim10', 'trim25', 'huber', 'clip_mean']
fig, ax = plt.subplots(1, 2, figsize=(14, 4.5)); w = .13
for j, q in enumerate(('amp', 'ph')):
    base = np.array([st(est(ch, q, 'mean')) for ch in sc.CH])
    for k, e in enumerate(ests):
        v = np.array([st(est(ch, q, e)) for ch in sc.CH])
        ax[j].bar(np.arange(4) + (k - 2.5) * w, v / base, w, label=e)
    ax[j].set_xticks(range(4)); ax[j].set_xticklabels(lab)
    ax[j].set_ylabel('30-s scatter / scatter of plain mean'); ax[j].set_ylim(.7, 1.3)
    ax[j].set_title({'amp': 'Amplitude location estimators', 'ph': 'Absolute phase location estimators'}[q])
ax[0].legend(ncol=3, fontsize=7)
plt.tight_layout(); plt.savefig(G + '6_estimator_comparison.png'); plt.close()


# 7 ---- Allan deviation: single channel vs common-mode rejected
def adev(x, ms):
    o = []
    for m in ms:
        n = len(x) // m
        y = x[:n * m].reshape(n, m).mean(1)
        o.append(np.sqrt(.5 * np.mean(np.diff(y) ** 2)))
    return np.array(o)


ms = np.unique(np.round(np.logspace(0, np.log10(500), 24)).astype(int)); tau = ms * 30.
a1 = 1e6 * np.log(d['ch1_amp_mean']); a2 = 1e6 * np.log(d['ch2_amp_mean']); rat = a1 - a2
p1 = wrap_abs('ch1') * 1000; p2 = wrap_abs('ch2') * 1000
fig, ax = plt.subplots(1, 3, figsize=(16, 4.5))
ax[0].loglog(tau, adev(a1, ms), 'o-', label='ln amp ch1')
ax[0].loglog(tau, adev(a2, ms), 'o-', label='ln amp ch2')
ax[0].loglog(tau, adev(rat, ms), 's-', color='k', label='ln(amp1/amp2) ratiometric')
naive = 1e6 * (A[:, :, 1].std(1) / A[:, :, 1].mean(1)).mean() / np.sqrt(767)
ax[0].axhline(naive, color='k', ls=':', label='naive within-capture SE (ch1)')
ax[0].set_xlabel('averaging time (s, whole captures)'); ax[0].set_ylabel('Allan deviation (ppm)')
ax[0].legend(fontsize=7); ax[0].set_title('Amplitude stability')
ax[1].loglog(tau, adev(p1, ms), 'o-', label='phase ch1')
ax[1].loglog(tau, adev(p2, ms), 'o-', label='phase ch2')
ax[1].loglog(tau, adev(p1 - p2, ms), 's-', color='k', label='phase ch1-ch2')
ax[1].set_xlabel('averaging time (s)'); ax[1].set_ylabel('Allan deviation (mdeg)'); ax[1].legend(fontsize=7)
ax[1].set_title('Phase stability')
ax[2].plot(a1 - np.median(a1), a2 - np.median(a2), '.', ms=2)
ax[2].set_xlabel('ch1 ln-amp (ppm)'); ax[2].set_ylabel('ch2 ln-amp (ppm)')
ax[2].set_title(f'capture means ch1 vs ch2: r={np.corrcoef(a1, a2)[0, 1]:.3f}')
plt.tight_layout(); plt.savefig(G + '7_allan_common_mode.png'); plt.close()

# 8 ---- bursts on ch0/ch3
fig, ax = plt.subplots(2, 3, figsize=(16, 7))
for r, c in enumerate((0, 3)):
    k = d[f'ch{c}_amp_exkurt']; w_ = np.argsort(-k)[:2]
    ax[r, 0].plot(th, k, lw=.5, color=col[c]); ax[r, 0].set_yscale('symlog', linthresh=3)
    ax[r, 0].set_title(f'{lab[c]} per-capture amp excess kurtosis'); ax[r, 0].set_xlabel('time (h)')
    for j, i in enumerate(w_):
        ax[r, 1 + j].plot(A[i, :, c] - np.median(A[i, :, c]), lw=.7, color=col[c])
        ax[r, 1 + j].set_title(f'{lab[c]} capture {i} (t={th[i]:.1f} h, exkurt {k[i]:.1f})')
        ax[r, 1 + j].set_xlabel('cycle'); ax[r, 1 + j].set_ylabel('amp - median (counts)')
plt.tight_layout(); plt.savefig(G + '8_bursts_ch0_ch3.png'); plt.close()
print('done')
