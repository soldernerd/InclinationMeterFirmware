"""
S1/S2 raw for the first 95 minutes of this 2h office-desk test, axis-scale-
matched to Testing/2026-09-27_24hr_granite_plate_test/graphs/
displacement_first_95min.png so the two can be compared visually side by
side. This window is entirely within the office-desk baseline (not-charging)
phase -- forced charging in this test didn't start until t=60min... wait,
that's inside this window: charging starts at t=60min, so the last ~35min of
this 95min window IS the forced-charging phase of the original 2h test.
"""
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_2hr_charging_drift_test\data\long_drift_stream.csv"
OUT = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_2hr_charging_drift_test\graphs\displacement_first_95min_matched_scale.png"
T_MAX_MIN = 95.0

# matched to the granite plot's auto-scaled ylims, so both figures are
# directly comparable at a glance
S1_YLIM = (-0.0636446714401245, 0.15359666347503662)
S2_YLIM = (-0.2219409942626953, 0.35555124282836914)

t_min, d1, d2 = [], [], []
with open(CSV, newline="") as f:
    r = csv.DictReader(f)
    for row in r:
        t = float(row["t_s"]) / 60.0
        if t > T_MAX_MIN:
            break
        t_min.append(t)
        d1.append(float(row["delta1_mm_raw"]))
        d2.append(float(row["delta2_mm_raw"]))

fig, axes = plt.subplots(2, 1, figsize=(14, 6), sharex=True)

axes[0].plot(t_min, d1, color="tab:blue", linewidth=0.5)
axes[0].set_title("S1 delta1_mm_raw, office desk: first 95min (charging starts at t=60min)")
axes[0].set_ylabel("S1 raw (mm)")
axes[0].set_ylim(*S1_YLIM)
axes[0].axvline(60, color="red", linestyle="--", linewidth=1, label="forced charging starts")
axes[0].legend(loc="upper right")
axes[0].grid(True, alpha=0.3)

axes[1].plot(t_min, d2, color="tab:orange", linewidth=0.5)
axes[1].set_title("S2 delta2_mm_raw")
axes[1].set_ylabel("S2 raw (mm)")
axes[1].set_ylim(*S2_YLIM)
axes[1].axvline(60, color="red", linestyle="--", linewidth=1)
axes[1].set_xlabel("Time (minutes)")
axes[1].grid(True, alpha=0.3)

fig.tight_layout()
fig.savefig(OUT, dpi=110)
print(f"wrote {OUT}  ({len(t_min)} samples)")
