"""
Local noise of S1, S2 and S1-S2 before / after removing c*Re z, from the
24 h granite-plate log. Metrics:
  * per-batch scatter inside 1-minute windows (median over windows),
  * Allan deviation at several averaging times (tau), pooled over the quiet
    segments (0-245 min and 340-1050 min = 17.5 h; the charging transient and
    the sunlight period are excluded), plus the same over the whole 0-20 h.
c is the charging-start step coefficient (see correct_drift_with_ab_balance.py).
"""
import os
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
cols = ["t_s", "iA", "qA", "iB", "qB", "delta1_mm_raw", "delta2_mm_raw"]
d = pd.read_csv(os.path.join(HERE, "data", "granite_24hr.csv"), usecols=cols, low_memory=False)
d = d.dropna(subset=["iA", "iB"])
A = d.iA.values + 1j * d.qA.values
B = d.iB.values + 1j * d.qB.values
d["zre"] = (-(A + B) / (2 * (A - B))).real * 1e6
C1, C2 = 1.419, 1.424          # um/ppm, from the charging-start step
d["s1"] = d.delta1_mm_raw * 1e3
d["s2"] = d.delta2_mm_raw * 1e3
d["s1c"] = d.s1 - C1 * d.zre
d["s2c"] = d.s2 - C2 * d.zre
d["dif"] = d.s1 - d.s2
d["difc"] = d.s1c - d.s2c
d["t_min"] = d.t_s / 60.0

SERIES = [("S1", "s1", "s1c"), ("S2", "s2", "s2c"), ("S1-S2", "dif", "difc")]
QUIET = [(0, 245), (340, 1050)]                 # minutes
WHOLE = [(0, 1200)]


def in_segments(tmin, segs):
    m = np.zeros(len(tmin), bool)
    for a, b in segs:
        m |= (tmin >= a) & (tmin < b)
    return m


def per_batch_scatter(col, segs):
    """median over 1-minute windows of the std of the per-batch readings
    (after removing each window's own mean)"""
    x = d[in_segments(d.t_min.values, segs)]
    w = np.floor(x.t_min.values).astype(int)
    s = x.groupby(w)[col].std()
    return np.median(s.values)


def allan(col, tau_s, segs):
    x = d[in_segments(d.t_min.values, segs)]
    sec = np.floor(x.t_s.values).astype(np.int64)
    m = x.groupby(sec)[col].mean()
    idx = m.index.values
    vals = m.values
    sq, n = 0.0, 0
    for a, b in segs:
        s0, s1 = a * 60, b * 60
        sel = (idx >= s0) & (idx < s1)
        i, v = idx[sel], vals[sel]
        nb = int((s1 - s0) // tau_s)
        # mean per tau-window, skipping windows with too few seconds
        k = ((i - s0) // tau_s).astype(int)
        df = pd.DataFrame({"k": k, "v": v}).groupby("k").agg(m=("v", "mean"), c=("v", "size"))
        df = df[df.c >= 0.8 * tau_s]
        ks = df.index.values
        mm = df.m.values
        ok = np.diff(ks) == 1
        dd = np.diff(mm)[ok]
        sq += (dd ** 2).sum()
        n += len(dd)
    return np.sqrt(0.5 * sq / n)


print("Per-batch scatter inside 1-minute windows (median over windows, um):")
for segname, segs in (("quiet segments", QUIET), ("0-20 h", WHOLE)):
    print(f"  {segname}")
    for name, raw, cor in SERIES:
        print(f"    {name:6s} raw {per_batch_scatter(raw, segs):6.2f}   corrected {per_batch_scatter(cor, segs):6.2f}")

print("\nAllan deviation (um) vs averaging time, quiet segments:")
taus = [1, 10, 60, 300, 1800]
print(f"  {'tau':>6s} " + "".join(f"{n+' raw':>10s}{n+' cor':>10s}" for n, _, _ in SERIES))
for tau in taus:
    row = f"  {tau:5d}s "
    for name, raw, cor in SERIES:
        row += f"{allan(raw, tau, QUIET):10.2f}{allan(cor, tau, QUIET):10.2f}"
    print(row)
print("\nAllan deviation (um), whole 0-20 h (includes the charging transient):")
print(f"  {'tau':>6s} " + "".join(f"{n+' raw':>10s}{n+' cor':>10s}" for n, _, _ in SERIES))
for tau in taus:
    row = f"  {tau:5d}s "
    for name, raw, cor in SERIES:
        row += f"{allan(raw, tau, WHOLE):10.2f}{allan(cor, tau, WHOLE):10.2f}"
    print(row)
