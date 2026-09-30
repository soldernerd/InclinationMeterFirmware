"""
Correct the S1 / S2 drift with the A/B balance term Re z and report how much
S1, S2 and S1-S2 improve.

z = -(A+B)/(2*(A-B)) from the logged phasors (docs/signal_processing.tex,
section 12). The reading contains c * Re z, so the corrected reading is
    reading - c * Re z          (= dropping the B term of the ratio).

Two ways to get c per sensor:
  * EVENT-BASED (used for the plot): the sharp step at the start of charging
    (t ~ 247 min) changes z by ~42 ppm within minutes, faster than any real
    tilt change, so  c = delta(reading) / delta(Re z)  across that step.
    It is independent of the slow drift that is being corrected.
  * FITTED: least-squares slope of the reading on Re z over the whole run.
    Printed for comparison (it also absorbs any real tilt drift that happens
    to correlate with z), plus a half/half cross-validation.
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "data", "granite_24hr.csv")
# optional argument: only use the first CUT_H hours of the run (default: all)
CUT_H = float(sys.argv[1]) if len(sys.argv) > 1 else None
SUFFIX = f"_first{CUT_H:g}h" if CUT_H else ""
OUT = os.path.join(HERE, "graphs", f"drift_corrected_with_ab_balance{SUFFIX}.png")

cols = ["t_s", "iA", "qA", "iB", "qB", "delta1_mm_raw", "delta2_mm_raw"]
d = pd.read_csv(CSV, usecols=cols, low_memory=False).dropna(subset=["iA", "iB"])
if CUT_H:
    d = d[d.t_s < CUT_H * 3600.0]
A = d.iA.values + 1j * d.qA.values
B = d.iB.values + 1j * d.qB.values
z = -(A + B) / (2 * (A - B))
d["zre"] = z.real * 1e6                      # ppm
d["s1"] = d.delta1_mm_raw * 1000.0           # um
d["s2"] = d.delta2_mm_raw * 1000.0
g = d.groupby(np.floor(d.t_s / 60.0)).mean(numeric_only=True)   # 1-minute means
t_min = g.index.values
t = t_min / 60.0                              # hours

# --- event-based coefficient (charging-start step) ------------------------
pre = (t_min >= 235) & (t_min < 245)
post = (t_min >= 255) & (t_min < 265)
dz = g.zre.values[post].mean() - g.zre.values[pre].mean()
c = {n: (g[n].values[post].mean() - g[n].values[pre].mean()) / dz for n in ("s1", "s2")}
print(f"Charging-start step: dRe z = {dz:+.1f} ppm; coefficient "
      f"S1 = {c['s1']:.3f} um/ppm, S2 = {c['s2']:.3f} um/ppm")

# --- fitted coefficient, for comparison ------------------------------------
for n in ("s1", "s2"):
    beta = np.polyfit(g.zre.values, g[n].values, 1)[0]
    r = np.corrcoef(g.zre.values, g[n].values)[0, 1]
    print(f"{n.upper()}: fitted slope over whole run = {beta:.3f} um/ppm, corr = {r:+.3f}")
half = len(g) // 2
for tr, te, lab in ((slice(0, half), slice(half, None), "fit 0-12 h -> test 12-24 h"),
                    (slice(half, None), slice(0, half), "fit 12-24 h -> test 0-12 h")):
    out = []
    for n in ("s1", "s2"):
        b = np.polyfit(g.zre.values[tr], g[n].values[tr], 1)[0]
        raw = g[n].values[te]
        out.append(f"{n.upper()} beta {b:.2f}: std {raw.std():.1f} -> {(raw - b * g.zre.values[te]).std():.1f}")
    print(f"  cross-validation {lab}: " + " | ".join(out))


def stats(y):
    y = y - y[:10].mean()
    tr = np.polyfit(t, y, 1)
    return y.max() - y.min(), y.std(), tr[0], np.std(y - np.polyval(tr, t))


s1c = g.s1.values - c["s1"] * g.zre.values
s2c = g.s2.values - c["s2"] * g.zre.values
rows = [("S1 raw", g.s1.values), ("S1 corrected", s1c),
        ("S2 raw", g.s2.values), ("S2 corrected", s2c),
        ("S1-S2 raw", g.s1.values - g.s2.values), ("S1-S2 corrected", s1c - s2c)]
print(f"\n{'':18s}{'p-p (um)':>10s}{'std (um)':>10s}{'trend (um/h)':>14s}{'detrended rms':>15s}")
for lab, y in rows:
    p = stats(y)
    print(f"{lab:18s}{p[0]:10.1f}{p[1]:10.1f}{p[2]:14.2f}{p[3]:15.1f}")

fig, ax = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
for a, (raw, cor, title, color) in zip(ax[:2], ((g.s1.values, s1c, "S1", "tab:blue"),
                                                (g.s2.values, s2c, "S2", "tab:green"))):
    a.plot(t, raw - raw[:10].mean(), color=color, alpha=0.5, label=f"{title} raw")
    a.plot(t, cor - cor[:10].mean(), color="k", lw=1.0,
           label=f"{title} corrected (c = {c[title.lower()]:.2f} µm/ppm)")
    a.set_ylabel(f"{title} drift (µm)")
    a.legend(loc="upper right", fontsize=8)
    a.grid(alpha=0.3)
dr = g.s1.values - g.s2.values
dc = s1c - s2c
ax[2].plot(t, dr - dr[:10].mean(), color="tab:red", alpha=0.5, label="S1-S2 raw")
ax[2].plot(t, dc - dc[:10].mean(), color="k", lw=1.0, ls="--", label="S1-S2 corrected (identical: z is common mode)")
ax[2].set_ylabel("S1-S2 drift (µm)")
ax[2].set_xlabel("Time (h)")
ax[2].legend(loc="upper right", fontsize=8)
ax[2].grid(alpha=0.3)
ax[0].set_title("Drift of S1, S2 and S1-S2 before / after removing the A/B balance term"
               + (f" (first {CUT_H:g} h)" if CUT_H else " (24 h run)"))
fig.tight_layout()
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=130)
print("\nwrote", OUT)
