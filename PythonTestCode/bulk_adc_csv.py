#!/usr/bin/env python3
"""
Pull one bulk raw-ADC capture off the InclinationMeter and write it to CSV.

The device samples all 4 ADS131M04 channels at the full 20833.33 Hz into a
RAM buffer (Config/config.h ADC_BULK_SAMPLE_COUNT, default 4096 samples =
~197 ms), then streams the buffer out in chunks over the wire at whatever
speed the link allows -- transport speed no longer limits sample rate
(docs/api-v2-spec.md §4.5).

  python bulk_adc_csv.py                       # auto-detect port, -> bulk_adc.csv
  python bulk_adc_csv.py --port COM6 --out cap.csv
  python bulk_adc_csv.py --volts              # extra columns in volts

CSV columns: sample,ch0,ch1,ch2,ch3   (raw signed 24-bit codes; +_v if --volts)

Install:  pip install pyserial
"""

import argparse
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Need pyserial:  pip install pyserial")

import apiv2 as a

BAUD = 115200
_KNOWN_VID_PID = {
    (0x0483, 0x374B), (0x0483, 0x374E), (0x0483, 0x374F), (0x0483, 0x3752),
    (0x0403, 0x6001), (0x0403, 0x6015), (0x10C4, 0xEA60),
}


def find_port():
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid is not None and (p.vid, p.pid) in _KNOWN_VID_PID:
            return p.device
    if len(ports) == 1:
        return ports[0].device
    if ports:
        sys.exit("Multiple serial ports; pass --port:\n  " +
                 "\n  ".join(f"{p.device}  {p.description}" for p in ports))
    sys.exit("No serial ports found.")


def modular_gap(prev, cur):
    """True if `cur` is not exactly prev+1 (mod 256)."""
    return prev is not None and cur != ((prev + 1) & 0xFF)


def capture(ser, timeout=20.0):
    reasm = a.Reassembler()
    op = a.OP_BULK_RAW_ADC_START

    ser.reset_input_buffer()
    ser.write(a.build(op))

    rows = []
    acked = False
    last_page = None
    gaps = 0
    deadline = time.time() + timeout

    while len(rows) < a.ADC_BULK_SAMPLE_COUNT:
        if time.time() > deadline:
            ser.write(a.build(a.OP_BULK_RAW_ADC_CANCEL))
            raise TimeoutError(
                f"gave up after {timeout:.0f}s with {len(rows)}/"
                f"{a.ADC_BULK_SAMPLE_COUNT} samples")

        chunk = ser.read(4096)
        if not chunk:
            continue
        for pkt_op, status, data in reasm.feed(chunk):
            if pkt_op != op:
                continue                      # unrelated (e.g. a stray log frame)
            if status is None:
                gaps += 1                     # bad CRC on this packet
                continue
            if not acked:
                if status != 0x00:
                    raise RuntimeError(
                        f"START_BULK NACK: {a.STATUS.get(status, status)}")
                acked = True
                continue
            if status != 0x00:
                continue
            page, samples = a.decode_bulk_adc_chunk(data)
            if modular_gap(last_page, page):
                gaps += 1
            last_page = page
            rows.extend(samples)

    return rows[:a.ADC_BULK_SAMPLE_COUNT], gaps


def summarise(rows):
    n = len(rows)
    print(f"  {n} samples x 4 channels")
    for ch in range(4):
        xs = [r[ch] for r in rows]
        mn, mx = min(xs), max(xs)
        mean = sum(xs) / n
        rms = (sum((x - mean) ** 2 for x in xs) / n) ** 0.5
        p2p_v = (mx - mn) * a.ADC_RAW_LSB_V
        tag = "  <- flat / no sensor" if (mx - mn) < 64 else ""
        print(f"  ch{ch}: min={mn:>10}  max={mx:>10}  mean={mean:>12.1f}  "
              f"rms={rms:>10.1f}  p2p={p2p_v*1000:8.2f} mV{tag}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default="bulk_adc.csv")
    ap.add_argument("--volts", action="store_true",
                    help="add ch0_v..ch3_v columns in volts")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Opening {port} @ {BAUD} ...")
    with serial.Serial(port, BAUD, timeout=0.2) as ser:
        t0 = time.time()
        rows, gaps = capture(ser, timeout=args.timeout)
        dt = time.time() - t0

    print(f"  transfer done in {dt:.1f}s, {gaps} gap/CRC event(s)")
    summarise(rows)

    with open(args.out, "w", newline="") as f:
        if args.volts:
            f.write("sample,ch0,ch1,ch2,ch3,ch0_v,ch1_v,ch2_v,ch3_v\n")
            for i, r in enumerate(rows):
                v = [x * a.ADC_RAW_LSB_V for x in r]
                f.write(f"{i},{r[0]},{r[1]},{r[2]},{r[3]},"
                        f"{v[0]:.9f},{v[1]:.9f},{v[2]:.9f},{v[3]:.9f}\n")
        else:
            f.write("sample,ch0,ch1,ch2,ch3\n")
            for i, r in enumerate(rows):
                f.write(f"{i},{r[0]},{r[1]},{r[2]},{r[3]}\n")
    print(f"  wrote {args.out}")


if __name__ == "__main__":
    main()
