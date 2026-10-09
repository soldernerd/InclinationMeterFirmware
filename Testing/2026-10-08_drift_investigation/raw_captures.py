"""Raw 24-bit ADC captures (4 channels, 6144 samples = 295 ms at 20833.33 Hz) in a sequence of excitation states, for spectral analysis.
Needs fw >= 0.10.91 (Commands 0x0A excitation, 0x0B ADC mux, 0x0C n) and the demod STOPPED (this script stops it and restarts it at the end).
Run it only when no other script holds COM5.

States (name, n = samples per cycle, excitation on/off, ADC input short on ch0 + ch3, captures):
  n08_on x8, n08_off x4, n08_short_ch0_ch3 x3, n07 / n09 / n10 / n11 / n12 / n13 / n16 (on) x4 each, n08_on_again x4.
Output: data/raw_captures_<time>.npz  (data[capture, sample, channel] int32 codes; ch0 = S2, ch1 = B, ch2 = A, ch3 = S1; plus name / n / exc / short per capture).
python raw_captures.py [--port COM5]   then   python raw_spectra.py <npz>"""
import argparse
import os
import struct
import sys
import time
from datetime import datetime

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "PythonTestCode"))
import apiv2 as a            # noqa: E402
import bulk_adc_csv as bulk  # noqa: E402
import serial                # noqa: E402

OP_EXC = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0A)
OP_MUX = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0B)
OP_NFREQ = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0C)

STATES = [("n08_on", 8, 1, False, 8), ("n08_off", 8, 0, False, 4), ("n08_short_ch0_ch3", 8, 1, True, 3),
          ("n07_on", 7, 1, False, 4), ("n09_on", 9, 1, False, 4), ("n10_on", 10, 1, False, 4), ("n11_on", 11, 1, False, 4),
          ("n12_on", 12, 1, False, 4), ("n13_on", 13, 1, False, 4), ("n16_on", 16, 1, False, 4), ("n08_on_again", 8, 1, False, 4)]


def log(m):
    print(f"{datetime.now().strftime('%H:%M:%S')} {m}", flush=True)


def cmd(ser, reasm, op, payload=b"", what=""):
    ser.write(a.build(op, payload))
    end = time.time() + 3.0
    while time.time() < end:
        chunk = ser.read(256)
        for o, st, d in (reasm.feed(chunk) if chunk else []):
            if o == op:
                if st != 0:
                    log(f"  {what}: refused ({a.STATUS.get(st, st)})")
                return st
    log(f"  {what}: no answer")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="auto")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    port = bulk.find_port() if args.port == "auto" else args.port
    out = args.out or os.path.join(HERE, "data", datetime.now().strftime("raw_captures_%Y%m%d_%H%M%S.npz"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()
    reasm = a.Reassembler()
    cmd(ser, reasm, a.OP_CMD_SIGNAL_ANALYSIS, bytes([0]), "stop demod")
    time.sleep(0.5)
    data, names, ns, excs, shorts, ts = [], [], [], [], [], []
    cur_n, cur_short = 8, False
    try:
        for name, n, exc, short, count in STATES:
            if n != cur_n:
                cmd(ser, reasm, OP_NFREQ, bytes([n]), f"n={n}")
                cur_n = n
            if short != cur_short:
                cmd(ser, reasm, OP_MUX, bytes([0x09 if short else 0x0F, 1 if short else 0]), "mux")
                cur_short = short
            cmd(ser, reasm, OP_EXC, struct.pack("<BH", exc, 0), "excitation")
            time.sleep(1.5)
            for k in range(count):
                ser.reset_input_buffer()
                try:
                    rows, gaps = bulk.capture(ser, timeout=25.0)
                except Exception as e:
                    log(f"  {name} #{k}: capture failed: {e!r}")
                    continue
                data.append(np.array(rows, dtype=np.int32)); names.append(name); ns.append(n); excs.append(exc); shorts.append(int(short)); ts.append(time.time())
                log(f"  {name} #{k}: {len(rows)} samples, {gaps} gap/CRC events")
                time.sleep(0.3)
    finally:
        log("restoring: n = 8, excitation ON, inputs normal, demod running")
        cmd(ser, reasm, OP_EXC, struct.pack("<BH", 1, 0), "excitation")
        cmd(ser, reasm, OP_NFREQ, bytes([8]), "n=8")
        cmd(ser, reasm, OP_MUX, bytes([0x0F, 0]), "mux")
        cmd(ser, reasm, a.OP_CMD_SIGNAL_ANALYSIS, bytes([1]), "start demod")
        ser.close()
        if data:
            np.savez(out, data=np.array(data), name=np.array(names), n=np.array(ns), exc=np.array(excs), short=np.array(shorts), t=np.array(ts))
            log(f"saved {len(data)} captures -> {out}")


if __name__ == "__main__":
    main()
