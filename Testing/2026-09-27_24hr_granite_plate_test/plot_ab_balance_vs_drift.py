"""
24 h granite-plate run: A/B balance drift vs S1 / S2 reading drift vs board
temperature, all on one time axis (1-minute means).

A/B balance is z = -(A+B) / (2*(A-B)) from the logged phasors (the term of
the ratio x = (S/k - B)/(A - B) that depends only on A and B, see
docs/signal_processing.tex section 12). Re z is the amplitude part of the
imbalance (|B|/|A|), Im z the phase part. Drift = change relative to the
first 10 minutes of the run.
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "data", "granite_24hr.csv")
OUT = os.path.join(HERE, "graphs", "ab_balance_vs_drift.png")
CHARGE_START_MIN = 246.86        # CHARGE_START event in the log

cols = ["t_s", "iA", "qA", "iB", "qB", "delta1_mm_raw", "delta2_mm_raw",
        "onboard_temp_cdeg", "usb", "charging"]
d = pd.read_csv(CSV, usecols=cols, low_memory=False)
d = d.dropna(subset=["iA", "iB"])
A = d.iA.values + 1j * d.qA.values
B = d.iB.values + 1j * d.qB.values
z = -(A + B) / (2 * (A - B))
d["zre_ppm"] = z.real * 1e6
d["zim_ppm"] = z.imag * 1e6
d["T"] = d.onboard_temp_cdeg.ffill() / 100.0
d["s1_um"] = d.delta1_mm_raw * 1000.0
d["s2_um"] = d.delta2_mm_raw * 1000.0
d["t_min"] = d.t_s / 60.0
d["A_abs"] = np.abs(A)
d["B_abs"] = np.abs(B)

g = d.groupby(np.floor(d.t_min)).mean(numeric_only=True)
t_h = g.t_min / 60.0
base = g[g.t_min < 10]
for c in ("zre_ppm", "zim_ppm", "s1_um", "s2_um"):
    g[c + "_drift"] = g[c] - base[c].mean()
for c in ("A_abs", "B_abs"):
    g[c + "_drift"] = (g[c] / base[c].mean() - 1.0) * 1e6      # ppm of the start value

fig, ax = plt.subplots(6, 1, figsize=(11, 14), sharex=True,
                       gridspec_kw={"height_ratios": [1, 1.2, 1.2, 1.2, 1.2, 1.2]})

ax[0].plot(t_h, g["T"], color="tab:red")
ax[0].set_ylabel("Board temp (°C)")
ax[0].set_title("24 h granite-plate run: A/B balance vs S1/S2 drift vs temperature")

ax[1].plot(t_h, g.zre_ppm_drift, color="tab:purple", label="Re z (amplitude part, |B|/|A| / 4)")
ax[1].plot(t_h, g.zim_ppm_drift, color="tab:gray", lw=0.9, label="Im z (phase part)")
ax[1].set_ylabel("A/B balance drift (ppm)")
ax[1].legend(loc="upper right", fontsize=8)

ax[2].plot(t_h, g.A_abs_drift, color="tab:orange")
ax[2].set_ylabel("|A| drift (ppm)")

ax[3].plot(t_h, g.B_abs_drift, color="tab:brown")
ax[3].set_ylabel("|B| drift (ppm)")

ax[4].plot(t_h, g.s1_um_drift, color="tab:blue")
ax[4].set_ylabel("S1 reading drift (µm)")

ax[5].plot(t_h, g.s2_um_drift, color="tab:green")
ax[5].set_ylabel("S2 reading drift (µm)")
ax[5].set_xlabel("Time (h)")

for a in ax:
    a.axvline(CHARGE_START_MIN / 60.0, color="k", ls="--", lw=0.8)
    a.grid(alpha=0.3)
ax[0].text(CHARGE_START_MIN / 60.0 + 0.1, ax[0].get_ylim()[1] - 0.6,
           "charging starts", fontsize=8, va="top")

fig.tight_layout()
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=130)
print("wrote", OUT)

for c in ("s1_um", "s2_um"):
    print(f"corr(Re z, {c}) = {np.corrcoef(g.zre_ppm, g[c])[0, 1]:+.3f}   "
          f"corr(T, {c}) = {np.corrcoef(g['T'], g[c])[0, 1]:+.3f}")
print(f"corr(Re z, T) = {np.corrcoef(g.zre_ppm, g['T'])[0, 1]:+.3f}")
for c in ("A_abs", "B_abs"):
    print(f"{c}: range {g[c + '_drift'].max() - g[c + '_drift'].min():.0f} ppm, "
          f"corr with T {np.corrcoef(g[c], g['T'])[0, 1]:+.3f}")
