"""
S1, S2, and differential drift trend, all three on one chart (binned means,
not raw per-batch noise -- the point here is the slow trend, which the raw
20Hz stream would just bury in noise). Binned at BIN_MIN-minute resolution
over the whole run collected so far.
"""
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CSV = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_24hr_granite_plate_test\data\granite_24hr.csv"
OUT = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_24hr_granite_plate_test\graphs\drift_combined.png"
BIN_MIN = 1.0

t, d1, d2, dd, temp = [], [], [], [], []
with open(CSV, newline="") as f:
    r = csv.DictReader(f)
    for row in r:
        t.append(float(row["t_s"]) / 60.0)
        d1.append(float(row["delta1_mm_raw"]))
        d2.append(float(row["delta2_mm_raw"]))
        dd.append(float(row["delta_diff_mm_raw"]))
        temp.append(float(row["onboard_temp_cdeg"]) / 100.0 if row["onboard_temp_cdeg"] else np.nan)

t = np.array(t); d1 = np.array(d1); d2 = np.array(d2); dd = np.array(dd); temp = np.array(temp)
tmax = t[-1]
edges = np.arange(0, tmax + BIN_MIN, BIN_MIN)
bin_centers, b1, b2, bd, btemp = [], [], [], [], []
for i in range(len(edges) - 1):
    mask = (t >= edges[i]) & (t < edges[i + 1])
    if mask.sum() < 5:
        continue
    bin_centers.append((edges[i] + edges[i + 1]) / 2)
    b1.append(d1[mask].mean())
    b2.append(d2[mask].mean())
    bd.append(dd[mask].mean())
    btemp.append(np.nanmean(temp[mask]))

# Charging window -- read straight from the logged charging flag rather than
# hardcoding, so this stays correct if the script is rerun later. CHARGE_START_MIN
# is a real, logged transition (t=246.86min, event=CHARGE_START); charging has
# not ended as of the latest sample, so there is no end line to draw yet -- the
# shaded span runs to the end of the data instead of a fixed end point.
charge_start_min = None
charge_end_min = None
prev_c = None
with open(CSV, newline="") as f:
    r = csv.DictReader(f)
    for row in r:
        c = row["charging"]
        if c == "":
            continue
        tm = float(row["t_s"]) / 60.0
        if prev_c is not None and c != prev_c:
            if prev_c == "0" and c == "1" and charge_start_min is None:
                charge_start_min = tm
            elif prev_c == "1" and c == "0" and charge_start_min is not None and charge_end_min is None:
                charge_end_min = tm
        prev_c = c
still_charging = (charge_start_min is not None and charge_end_min is None)

fig, ax = plt.subplots(figsize=(14, 7))

if charge_start_min is not None:
    span_end = charge_end_min if charge_end_min is not None else tmax
    ax.axvspan(charge_start_min, span_end, color="tab:red", alpha=0.07, zorder=0)
    ax.axvline(charge_start_min, color="tab:red", linestyle="--", linewidth=1.1,
               label=f"charging starts (t={charge_start_min:.0f}min)")
    if charge_end_min is not None:
        ax.axvline(charge_end_min, color="tab:red", linestyle=":", linewidth=1.1,
                   label=f"charging ends (t={charge_end_min:.0f}min)")

ax.plot(bin_centers, b1, color="tab:blue", linewidth=1.3, label="S1 (delta1_mm_raw)")
ax.plot(bin_centers, b2, color="tab:orange", linewidth=1.3, label="S2 (delta2_mm_raw)")
ax.plot(bin_centers, bd, color="tab:green", linewidth=1.3, label="Diff (delta1 - delta2)")
ax.set_xlabel("Time (minutes)")
ax.set_ylabel("mm")
title_suffix = " (still charging as of latest sample)" if still_charging else ""
ax.set_title(f"Granite plate: S1/S2/Diff drift + onboard temp, {BIN_MIN:.0f}-min bin means, "
             f"full run so far ({tmax:.0f}min){title_suffix}")
ax.grid(True, alpha=0.3)
ax.axhline(0, color="gray", linewidth=0.7, alpha=0.5)

ax2 = ax.twinx()
ax2.plot(bin_centers, btemp, color="tab:red", linewidth=1.1, alpha=0.85, label="Onboard temp")
ax2.set_ylabel("Onboard temp (C)", color="tab:red")
ax2.tick_params(axis="y", labelcolor="tab:red")

h1, l1 = ax.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, loc="upper left")

print(f"charging: start={charge_start_min} end={charge_end_min} still_charging={still_charging}")

fig.tight_layout()
fig.savefig(OUT, dpi=110)
print(f"wrote {OUT}  ({len(bin_centers)} bins from {len(t)} samples, {tmax:.1f}min)")
