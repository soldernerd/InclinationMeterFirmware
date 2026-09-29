"""
Same plot style as Testing/2026-09-27_2hr_charging_drift_test/graphs/
displacement_2hr_charging_test.gif (S1/S2 raw + onboard temp vs time), applied
to the first 95 minutes of the 24h granite-plate run, with the differential
(delta1-delta2) reading added as its own curve. No charging event to mark in
this window (USB connected throughout, charging stayed off) so the vertical
reference line from the original plot is omitted.
"""
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_24hr_granite_plate_test\data\granite_24hr.csv"
OUT = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_24hr_granite_plate_test\graphs\displacement_first_95min.png"
T_MAX_MIN = 95.0

t_min, d1, d2, ddiff, temp_t, temp_c = [], [], [], [], [], []
with open(CSV, newline="") as f:
    r = csv.DictReader(f)
    for row in r:
        t = float(row["t_s"]) / 60.0
        if t > T_MAX_MIN:
            break
        t_min.append(t)
        d1.append(float(row["delta1_mm_raw"]))
        d2.append(float(row["delta2_mm_raw"]))
        ddiff.append(float(row["delta_diff_mm_raw"]))
        if row["onboard_temp_cdeg"]:
            temp_t.append(t)
            temp_c.append(float(row["onboard_temp_cdeg"]) / 100.0)

fig, axes = plt.subplots(4, 1, figsize=(14, 12), sharex=True)

axes[0].plot(t_min, d1, color="tab:blue", linewidth=0.5)
axes[0].set_title("S1 delta1_mm_raw, granite plate: first 95min (USB connected, not charging)")
axes[0].set_ylabel("S1 raw (mm)")
axes[0].grid(True, alpha=0.3)

axes[1].plot(t_min, d2, color="tab:orange", linewidth=0.5)
axes[1].set_title("S2 delta2_mm_raw")
axes[1].set_ylabel("S2 raw (mm)")
axes[1].grid(True, alpha=0.3)

axes[2].plot(t_min, ddiff, color="tab:green", linewidth=0.5)
axes[2].set_title("Differential (delta1 - delta2) delta_diff_mm_raw")
axes[2].set_ylabel("Diff raw (mm)")
axes[2].grid(True, alpha=0.3)

axes[3].plot(temp_t, temp_c, color="tab:red", linewidth=1.0)
axes[3].set_title("Onboard temperature")
axes[3].set_ylabel("Onboard temp (C)")
axes[3].set_xlabel("Time (minutes)")
axes[3].grid(True, alpha=0.3)

fig.tight_layout()
fig.savefig(OUT, dpi=110)
print(f"wrote {OUT}  ({len(t_min)} fast samples, {len(temp_t)} temp samples)")
print("S1 ylim:", axes[0].get_ylim())
print("S2 ylim:", axes[1].get_ylim())
print("DIFF ylim:", axes[2].get_ylim())
