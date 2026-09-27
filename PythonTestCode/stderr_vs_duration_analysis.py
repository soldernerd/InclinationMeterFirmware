#!/usr/bin/env python3
"""Standard-error-vs-averaging-duration analysis, run against archived
Testing/ raw displacement CSVs (Topic 0x03 stream format: t_s,issue_seq,
delta1_mm_raw,residual1,delta2_mm_raw,residual2[,quality1_ok,quality2_ok]).

Answers two questions with real bench data instead of theory alone:
  1. How does standard error of an ABSOLUTE (single-channel) reading actually
     scale with averaging duration -- does it keep following 1/sqrt(N), or
     does un-ignorable drift make it plateau?
  2. How does a DIFFERENTIAL (S1-S2, one sensor fixed as reference, the other
     roving) reading compare -- does common-mode drift/noise cancellation
     help, and by how much?

See docs/wp10_displacement.md's "Standard error vs averaging duration" section
for the write-up this script's output feeds.
"""
import csv
import sys
import numpy as np

TESTING_DIR = r"J:\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing"

DATASETS = [
    ("2-hour test, baseline hour",
     TESTING_DIR + r"\2026-09-27_2hr_charging_drift_test\data\long_drift_stream.csv",
     lambda d: d["t"] < 3600),
    ("10-min streaming monitor (pre-swap)",
     TESTING_DIR + r"\2026-09-26_10min_streaming_monitor\data\monitor_10min_stream.csv",
     None),
    ("10-min sensor-swap test (post-swap)",
     TESTING_DIR + r"\2026-09-26_sensor_swap_test\data\monitor_10min_stream_swapped.csv",
     None),
]

BIN_SIZES = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000]


def load_stream(path):
    t, seq, d1, r1, d2, r2, q1, q2 = [], [], [], [], [], [], [], []
    with open(path, newline="") as f:
        rd = csv.reader(f)
        header = next(rd)
        has_q = "quality1_ok" in header
        for row in rd:
            t.append(float(row[0])); seq.append(int(row[1]))
            d1.append(float(row[2])); r1.append(float(row[3]))
            d2.append(float(row[4])); r2.append(float(row[5]))
            if has_q:
                q1.append(int(row[6])); q2.append(int(row[7]))
    out = dict(t=np.array(t), seq=np.array(seq), d1=np.array(d1), d2=np.array(d2))
    if has_q:
        out["q1"] = np.array(q1); out["q2"] = np.array(q2)
    return out


def autocorr(x, lag):
    x = x - x.mean()
    return float(np.dot(x[:len(x) - lag], x[lag:]) / np.dot(x, x))


def binned_std(x, bin_sizes):
    """Std of the bin-mean series for a range of bin (averaging-window)
    sizes -- the empirical way to check 1/sqrt(N) averaging against real
    data, since 1/f noise or drift breaks the naive prediction at long
    windows in a way no closed-form formula captures honestly."""
    res = []
    for b in bin_sizes:
        n_bins = len(x) // b
        if n_bins < 8:
            break
        trimmed = x[:n_bins * b].reshape(n_bins, b)
        res.append((b, n_bins, float(trimmed.mean(axis=1).std(ddof=1))))
    return res


def analyze(label, path, mask_fn):
    d = load_stream(path)
    mask = mask_fn(d) if mask_fn else np.ones(len(d["t"]), dtype=bool)
    tt, d1, d2 = d["t"][mask], d["d1"][mask], d["d2"][mask]
    rate = mask.sum() / (tt[-1] - tt[0])
    diff = d1 - d2

    print(f"\n=== {label} === (n={mask.sum()}, rate={rate:.2f}Hz)")
    for name, x in [("S1 (absolute)", d1), ("S2 (absolute)", d2), ("S1-S2 (differential)", diff)]:
        steps = np.diff(x)
        print(f"  {name}: step std={steps.std(ddof=1)*1000:.3f}um  "
              f"lag-1 autocorr={autocorr(steps, 1):.3f}")
        for b, nb, s in binned_std(x, BIN_SIZES):
            print(f"      N={b:>5} ({b/rate:>7.2f}s): std={s*1000:>8.4f} um")


if __name__ == "__main__":
    for label, path, mask_fn in DATASETS:
        analyze(label, path, mask_fn)
