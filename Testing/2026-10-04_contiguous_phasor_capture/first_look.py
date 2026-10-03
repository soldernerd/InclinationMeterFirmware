"""
First look at a contiguous phasor-log CSV written by phasor_capture.py.

  python first_look.py data/phasor_log_YYYYmmdd_HHMMSS.csv [--out graphs]

What it does (all on the contiguous 40.7 Hz batch series, no decimation):
  1. data quality: batch gaps (dropped cycles), usable contiguous segments
  2. the reading  y = 2*d0*Re(S/(k*(A-B)))  [um, nominal d0/k, as in the earlier analysis]
  3. spectrum of y per sensor: the ~20 Hz pendulum line (now properly resolved), vibration level
  4. precision-measurement strategies: repeatability (Allan-style: scatter between consecutive
     back-to-back windows) of 0.5-4 s estimates for
        boxcar mean | triangular | Hann | 2x/3x cascaded boxcars | median | trim25 |
        firmware-style quality-flag filtered mean | decimated-by-2.33 mean (reproduces the old
        20 Hz stream, as a control)
  5. repeatability vs window length, boxcar vs Hann
Prints tables and writes graphs (PNG). Nominal units; only ratios between strategies matter.
"""
import argparse
import os
import sys

import numpy as np
from scipy import signal, stats

FS_BATCH = 20833.3333 / 8.0 / 64.0      # 40.69 Hz
D0 = (439.0, 495.0)                      # mm, theoretical (S1, S2)
K = 480.0


def load(path):
    names = ["index", "t_s", "capture_idx", "batch_idx", "seq", "gap_cycles", "first_batch",
             "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2",
             "onboard_temp_cdeg", "soc_pct", "charging", "usb", "label"]
    d = np.genfromtxt(path, delimiter=",", names=True, dtype=None, encoding=None)
    return {n: d[n] for n in d.dtype.names}


def segments(d):
    """Contiguous runs of batches: same capture, gap_cycles == 0, first batch of each capture dropped."""
    keep = d["first_batch"] == 0
    cap = d["capture_idx"]
    gap = d["gap_cycles"] != 0
    seg_id = np.zeros(len(cap), int)
    sid = 0
    for i in range(1, len(cap)):
        if cap[i] != cap[i - 1] or gap[i]:
            sid += 1
        seg_id[i] = sid
    segs = []
    for s in np.unique(seg_id):
        idx = np.where((seg_id == s) & keep)[0]
        if len(idx) > 1:
            segs.append(idx)
    return segs


def readings(d):
    A = d["iA"] + 1j * d["qA"]
    B = d["iB"] + 1j * d["qB"]
    S1 = d["iS1"] + 1j * d["qS1"]
    S2 = d["iS2"] + 1j * d["qS2"]
    x1 = S1 / K / (A - B)
    x2 = S2 / K / (A - B)
    return 2 * D0[0] * 1000 * x1.real, 2 * D0[1] * 1000 * x2.real, x1.imag, x2.imag


# ---------------- window / estimator definitions (weights over N consecutive batches) -------
def w_box(n):
    return np.ones(n)


def w_tri(n):
    return signal.windows.triang(n)


def w_hann(n):
    return signal.windows.hann(n + 2)[1:-1]


def w_casc(n, stages):
    L = max(1, int(round((n + stages - 1) / stages)))
    h = np.array([1.0])
    for _ in range(stages):
        h = np.convolve(h, np.ones(L))
    return h


def flag_good(res, mult=3.0, ewma=32):
    """Firmware quality flag: residual step > mult x EWMA baseline of good steps -> bad."""
    good = np.ones(len(res), bool)
    base = None
    for k in range(1, len(res)):
        s = abs(res[k] - res[k - 1])
        if base is None:
            base = s
            continue
        good[k] = s <= mult * base
        if good[k]:
            base += (s - base) / ewma
    return good


def window_estimates(segs, y, res, n, est):
    """Non-overlapping windows of n batches inside each segment -> list of per-segment estimate arrays."""
    out = []
    for idx in segs:
        m = len(idx) // n
        if m < 2:
            continue
        yy = y[idx][:m * n].reshape(m, n)
        if est == "decim2.33":      # every ~2.33rd batch, like the old stream
            rng = np.random.default_rng(0)
            e = []
            for row in yy:
                k, pick = rng.integers(0, 3), []
                while k < n:
                    pick.append(k)
                    k += rng.choice([2, 2, 3])
                e.append(row[pick].mean())
            out.append(np.array(e))
        elif est == "flag":
            g = flag_good(res[idx])[:m * n].reshape(m, n)
            e = np.array([r[gg].mean() if gg.any() else r.mean() for r, gg in zip(yy, g)])
            out.append(e)
        elif est == "median":
            out.append(np.median(yy, 1))
        elif est == "trim25":
            out.append(stats.trim_mean(yy, 0.25, axis=1))
        else:
            w = {"box": w_box, "tri": w_tri, "hann": w_hann,
                 "casc2": lambda n: w_casc(n, 2), "casc3": lambda n: w_casc(n, 3)}[est](n)
            w = w[:n] / w[:n].sum() if len(w) >= n else np.pad(w, (0, n - len(w))) / w.sum()
            out.append(yy @ w)
    return out


def adev(estimates):
    d = np.concatenate([np.diff(e) for e in estimates if len(e) > 1])
    return np.sqrt(0.5 * np.mean(d ** 2)) if len(d) else np.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--out", default="graphs")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    d = load(args.csv)
    n_rows = len(d["index"])
    segs = segments(d)
    lens = np.array([len(s) for s in segs])
    print(f"{n_rows} batches, {len(np.unique(d['capture_idx']))} captures, "
          f"{int((d['gap_cycles'] != 0).sum())} batch gaps; {len(segs)} contiguous segments, "
          f"median length {np.median(lens):.0f} batches ({np.median(lens)/FS_BATCH:.1f} s), "
          f"{lens.sum()/n_rows*100:.0f}% of batches usable")
    y1, y2, r1, r2 = readings(d)
    ydiff = y1 - y2

    # ---- 3. spectrum
    fig, ax = plt_setup(1, 2, (13, 4.5))
    for name, y, c in (("S1", y1, "tab:purple"), ("S2", y2, "tab:green"), ("S1-S2", ydiff, "k")):
        ps = []
        for idx in segs:
            if len(idx) >= 256:
                f, p = signal.welch(y[idx] - y[idx].mean(), fs=FS_BATCH, nperseg=256, window="hann")
                ps.append(p)
        if not ps:
            continue
        pm = np.mean(ps, 0)
        ax[0].semilogy(f, np.sqrt(pm), color=c, label=name)
        band = (f > 12) & (f < 20.3)
        print(f"  {name}: spectral peak {f[band][np.argmax(pm[band])]:.2f} Hz "
              f"({10*np.log10(pm[band].max()/np.median(pm[f > 5])):.0f} dB above median floor)")
    ax[0].set_xlabel("Hz (batch-stream, Nyquist 20.3 Hz)")
    ax[0].set_ylabel("um/sqrt(Hz)")
    ax[0].legend()
    ax[0].set_title("Contiguous batch-series spectrum")

    # ---- 4/5. strategies
    ests = ["box", "tri", "hann", "casc2", "casc3", "median", "trim25", "flag", "decim2.33"]
    Ts = [0.5, 1.0, 2.0, 2.5, 3.0, 4.0]
    print("\nrepeatability (um, scatter between back-to-back windows) -- S1 | S2 | S1-S2")
    results = {}
    for T in Ts:
        n = int(round(T * FS_BATCH))
        print(f" T = {T} s ({n} batches)")
        for e in ests:
            r = []
            for y, rs in ((y1, r1), (y2, r2), (ydiff, r1)):
                r.append(adev(window_estimates(segs, y, rs, n, e)))
            results[(T, e)] = r
            print(f"   {e:10s} " + "  ".join(f"{v:8.4f}" for v in r))
    for ci, name in enumerate(("S1", "S2", "S1-S2")):
        for e, ls in (("box", "-"), ("hann", "--"), ("casc3", ":")):
            ax[1].loglog(Ts, [results[(T, e)][ci] for T in Ts], ls, marker="o", label=f"{name} {e}")
    ax[1].set_xlabel("window length (s)")
    ax[1].set_ylabel("repeatability (um)")
    ax[1].legend(fontsize=6, ncol=2)
    ax[1].set_title("Repeatability vs window length")
    fig.tight_layout()
    fig.savefig(os.path.join(args.out, "first_look.png"), dpi=110)
    print(f"\nwrote {os.path.join(args.out, 'first_look.png')}")


def plt_setup(r, c, size):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"axes.grid": True, "grid.alpha": .3, "font.size": 9})
    fig, ax = plt.subplots(r, c, figsize=size)
    return fig, ax


if __name__ == "__main__":
    main()
