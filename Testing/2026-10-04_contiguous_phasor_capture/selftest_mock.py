"""
Hardware-free self-test of phasor_capture.py + first_look.py: replaces the serial
link with a simulated device (pendulum + white noise on the sensor phasors, a dropped-cycle
gap in one capture, a stale first batch) and runs the real capture loop.

  python selftest_mock.py          # writes data/selftest.csv, then run first_look.py on it
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import phasor_capture as pc   # noqa: E402

rng = np.random.default_rng(3)
F0 = pc.F0
BATCH = 64


def simulate_capture(gap_at=None):
    """512 batches of 64 cycles: sensors = tilt + 19.9 Hz resonator (driven by noise) + white noise."""
    n_cyc = pc.LOG_DEPTH * BATCH
    t = np.arange(n_cyc) / F0
    # resonator: AR(2), f=19.9 Hz, Q~5
    w0 = 2 * np.pi * 19.9 / F0
    r = np.exp(-w0 / (2 * 5.0))
    a1, a2 = 2 * r * np.cos(w0), -r * r
    e = rng.normal(0, 1, n_cyc) * 1.5e3
    pend = np.zeros(n_cyc)
    for i in range(2, n_cyc):
        pend[i] = a1 * pend[i - 1] + a2 * pend[i - 2] + e[i]
    unit = 65536.0
    # firmware-like per-cycle phasors (in 'cycle sums' scale), per channel
    A = (4.4e6 + rng.normal(0, 200, n_cyc)) * np.exp(1j * 0.3) * unit * 0.5
    B = -A * (1 + 1e-3)
    S1 = (9.0e5 + pend * 0.4 + rng.normal(0, 2000, n_cyc)) * np.exp(1j * (0.3 + np.pi)) * unit * 0.5
    S2 = (2.0e5 + pend * 1.2 + rng.normal(0, 3300, n_cyc)) * np.exp(1j * (0.3 + 1.1)) * unit * 0.5
    ents = []
    seq = BATCH - 1
    for b in range(pc.LOG_DEPTH):
        sl = slice(b * BATCH, (b + 1) * BATCH)
        if gap_at is not None and b >= gap_at:
            seq_b = (seq + 500) & 0xFFFF       # 500 cycles dropped on the device
        else:
            seq_b = seq
        ent = dict(seq=seq_b)
        for nm, X in (("A", A), ("B", B), ("S1", S1), ("S2", S2)):
            s = X[sl].sum()
            ent["i" + nm], ent["q" + nm] = float(s.real), float(s.imag)
        ents.append(ent)
        seq = (seq + BATCH) & 0xFFFF
    return ents


class MockLink:
    n = 0

    def __init__(self, port):
        self.ser = type("S", (), {"close": lambda self: None})()

    def req(self, op, payload=b"", timeout=2.0):
        return 0, b""

    def capture(self, timeout=0):
        MockLink.n += 1
        return simulate_capture(gap_at=200 if MockLink.n == 2 else None), 0


pc.Link = MockLink
pc.slow_poll = lambda link: (2650, 80, 0, 1)
pc.a.decode_identity = lambda d: "MOCK DEVICE"
sys.argv = ["phasor_capture.py", "--port", "MOCK", "--duration-min", "0.03", "--label", "selftest",
            "--out", os.path.join(HERE, "data", "selftest.csv")]
pc.main()
