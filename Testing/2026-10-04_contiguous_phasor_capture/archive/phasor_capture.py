"""
Contiguous 64-cycle phasor capture (2026-10-03, for the 2026-10-04 test).

Why: every earlier long-term log of the displacement readings was either a
decimated stream (Topic 0x03/0x02 at ~17 Hz, which aliases the ~20 Hz pendulum
into noise) or 0.3 s raw-ADC captures. The smoothing-filter / precision-
measurement design needs the *contiguous* 40.7 Hz series of 64-cycle batch
phasors. Firmware >= 0.10.63 stores every batch in its phasor log
(DISPLACEMENT_PHASOR_LOG_DECIMATION = 1): 512 entries = ~12.6 s contiguous per
capture. This script runs captures back-to-back (or every --interval-s) and
writes ONE flat CSV, one row per batch.

Each CSV row = one 64-cycle batch:
  index, t_s, capture_idx, batch_idx, seq, gap_cycles, first_batch,
  iB,qB,iA,qA,iS1,qS1,iS2,qS2   (the firmware's raw int64 batch sums, as float32)
  onboard_temp_cdeg, soc_pct, charging, usb, label
  * t_s       = host seconds since script start at the END of this batch
                (capture start + (seq+1)/2604.1667 s; seq counts cycles)
  * gap_cycles = cycles lost since the previous batch IN THE SAME CAPTURE
                (0 = contiguous; the seq counter advances for dropped cycles,
                so a gap shows up as a seq jump -- e.g. the LIVE-screen redraw
                drops ~500 cycles at a time; see docs/wp10_displacement.md)
  * first_batch = 1 for batch 0 of every capture: its first cycle can contain a
                stale ADC sample (Testing/2026-09-30_bulk_adc_30s_interval/findings.md
                section 1) -- drop it in analysis.
  The temp / SoC / charging fields are polled once per capture (before it starts).

Usage (examples):
  python phasor_capture.py --duration-min 10 --label "operator walking nearby"
  python phasor_capture.py --duration-min 60 --interval-s 30       # sparse, long
  python phasor_capture.py --duration-min 0.5                      # quick smoke test
Press Ctrl+C to stop early; the CSV is flushed after every capture.
By default the displacement demod is left STOPPED afterwards (it must be stopped
for a bulk capture); add --restore to restart it.
"""
import argparse
import csv
import os
import struct
import sys
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "PythonTestCode"))
import apiv2 as a   # noqa: E402
import serial       # noqa: E402

F0 = 20833.3333 / 8.0          # cycles per second (carrier / 8-sample cycle rate)
BATCH_CYCLES = 64              # must match Config/config.h DISPLACEMENT_BATCH_CYCLES
LOG_DEPTH = a.DISPLACEMENT_PHASOR_LOG_DEPTH   # 512
CAPTURE_TIMEOUT_S = 45.0

COLUMNS = ["index", "t_s", "capture_idx", "batch_idx", "seq", "gap_cycles", "first_batch",
           "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2",
           "onboard_temp_cdeg", "soc_pct", "charging", "usb", "label"]


def modular_gap(prev, cur):
    return prev is not None and cur != ((prev + 1) & 0xFF)


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.3)
        self.ser.reset_input_buffer()

    def req(self, op, payload=b"", timeout=2.0):
        self.ser.write(a.build(op, payload))
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk
                for op2, st, data in a.Reassembler().feed(buf):
                    if op2 == op:
                        return st, data
        return None, None

    def capture(self, timeout=CAPTURE_TIMEOUT_S):
        """One phasor-log capture -> (entries, link_gaps). entries = list of dicts."""
        reasm = a.Reassembler()
        op = a.OP_BULK_PHASORS_START
        self.ser.reset_input_buffer()
        self.ser.write(a.build(op))
        entries, gaps, acked, last_page = [], 0, False, None
        deadline = time.time() + timeout
        while len(entries) < LOG_DEPTH:
            if time.time() > deadline:
                self.ser.write(a.build(a.OP_BULK_PHASORS_CANCEL))
                return entries, gaps + 1000      # timeout: flagged with a large gap count
            chunk = self.ser.read(4096)
            if not chunk:
                continue
            for pkt_op, status, data in reasm.feed(chunk):
                if pkt_op != op:
                    continue
                if status is None:               # bad CRC / framing
                    gaps += 1
                    continue
                if not acked:
                    if status != 0x00:
                        raise RuntimeError(f"START_BULK phasors NACK: {a.STATUS.get(status, status)}")
                    acked = True
                    continue
                if status != 0x00:
                    continue
                page, ents = a.decode_phasor_log_chunk(data)
                if modular_gap(last_page, page):
                    gaps += 1
                last_page = page
                entries.extend(e for e in ents if e is not None)
        return entries[:LOG_DEPTH], gaps


def slow_poll(link):
    """temp (cdeg), soc, charging, usb -- None where unavailable."""
    temp = soc = chg = usb = None
    st, td = link.req(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
    if st == 0 and td and len(td) >= 2:
        temp = struct.unpack("<h", td[:2])[0]
    st, sd = link.req(a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_STATUS))
    if st == 0 and sd:
        s = a.decode_topic_status(sd)
        if s:
            soc, chg, usb = s["soc_pct"], int(s["charging"]), int(s["usb"])
    return temp, soc, chg, usb


def find_port():
    import serial.tools.list_ports as lp
    for p in lp.comports():
        if "STLink" in (p.description or "") or "ST-Link" in (p.description or ""):
            return p.device
    sys.exit("no ST-Link virtual COM port found; pass --port COMx")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="auto", help="serial port, or 'auto' = the ST-Link virtual COM port")
    ap.add_argument("--duration-min", type=float, default=10.0)
    ap.add_argument("--interval-s", type=float, default=0.0,
                    help="0 = back-to-back (default); else start a capture every N s")
    ap.add_argument("--out", default=None)
    ap.add_argument("--label", default="")
    ap.add_argument("--restore", action="store_true", help="restart the displacement demod at the end")
    ap.add_argument("--keep-autopoweroff", action="store_true",
                    help="do NOT disable auto power-off for the run (see below)")
    args = ap.parse_args()

    out = args.out or os.path.join(HERE, "data", datetime.now().strftime("phasor_log_%Y%m%d_%H%M%S.csv"))
    os.makedirs(os.path.dirname(out), exist_ok=True)

    port = find_port() if args.port == "auto" else args.port
    print("port:", port, flush=True)
    link = Link(port)
    st, d = link.req(a.OP_SYS_IDENTITY)
    if st != 0:
        sys.exit("IDENTITY failed -- check --port / that the board is awake")
    ident = a.decode_identity(d)
    print("IDENTITY:", ident, flush=True)

    st, d = link.req(a.OP_CMD_SIGNAL_ANALYSIS, bytes([0]))     # bulk captures need the demod stopped
    print("stop displacement demod ->", st, flush=True)

    # The auto power-off timer only pauses for USB / BLE / charging -- NOT for a wired-UART
    # session or a bulk capture -- so the board would drop into Standby mid-run after
    # auto_poweroff_s of encoder inactivity. Disable it for the run and restore it afterwards.
    getop = a.opcode(a.GET, a.CAT_SETTINGS, a.SET_AUTO_POWEROFF_S)
    setop = a.opcode(a.SET, a.CAT_SETTINGS, a.SET_AUTO_POWEROFF_S)
    saved_apo = None
    st, d = link.req(getop)
    if st == 0 and d and len(d) >= 2:
        cur = struct.unpack("<H", d[:2])[0]
        if cur != 0 and not args.keep_autopoweroff:
            st2, _ = link.req(setop, struct.pack("<H", 0))
            if st2 == 0:
                saved_apo = cur
            print(f"auto_poweroff_s {cur} -> 0 for the run [{a.STATUS.get(st2, st2)}] (restored at the end)", flush=True)
        elif cur != 0:
            print(f"WARNING: auto_poweroff_s = {cur} s: the board may go to Standby mid-capture "
                  f"unless USB/BLE is connected or it is charging", flush=True)

    f = open(out, "w", newline="")
    w = csv.writer(f)
    w.writerow(COLUMNS)
    t0 = time.time()
    t_end = t0 + args.duration_min * 60.0
    cap_idx = row_idx = 0
    tot_gaps = tot_bad = 0
    next_status = 60.0
    next_start = 0.0
    verified_contiguity = False
    print(f"writing {out}\n  duration {args.duration_min} min, "
          f"{'back-to-back' if args.interval_s <= 0 else f'every {args.interval_s}s'}", flush=True)

    try:
        while time.time() < t_end:
            now = time.time() - t0
            if args.interval_s > 0 and now < next_start:
                time.sleep(min(0.5, next_start - now))
                continue
            temp, soc, chg, usb = slow_poll(link)
            cap_t = time.time() - t0
            entries, link_gaps = link.capture()
            if len(entries) < LOG_DEPTH:
                tot_bad += 1
                print(f"  capture {cap_idx}: only {len(entries)}/{LOG_DEPTH} entries (link gaps {link_gaps})", flush=True)

            # firmware self-check: batches must be 64 cycles apart; 128 means decimation 2 (old firmware)
            if not verified_contiguity and len(entries) >= 3:
                step = (entries[2]["seq"] - entries[1]["seq"]) & 0xFFFF
                if step != BATCH_CYCLES:
                    f.close()
                    sys.exit(f"ABORT: consecutive batches are {step} cycles apart, expected {BATCH_CYCLES}. "
                             f"Firmware must be >= 0.10.63 (DISPLACEMENT_PHASOR_LOG_DECIMATION = 1).")
                verified_contiguity = True

            prev_seq = None
            cap_gaps = 0
            for b, e in enumerate(entries):
                gap = 0 if prev_seq is None else (((e["seq"] - prev_seq) & 0xFFFF) - BATCH_CYCLES)
                if gap:
                    cap_gaps += 1
                prev_seq = e["seq"]
                t_row = cap_t + (e["seq"] + 1) / F0
                w.writerow([row_idx, f"{t_row:.4f}", cap_idx, b, e["seq"], gap, 1 if b == 0 else 0,
                            repr(e["iB"]), repr(e["qB"]), repr(e["iA"]), repr(e["qA"]),
                            repr(e["iS1"]), repr(e["qS1"]), repr(e["iS2"]), repr(e["qS2"]),
                            "" if temp is None else temp, "" if soc is None else soc,
                            "" if chg is None else chg, "" if usb is None else usb, args.label])
                row_idx += 1
            f.flush()
            tot_gaps += cap_gaps
            if cap_gaps or link_gaps:
                print(f"  capture {cap_idx}: {cap_gaps} batch gaps (cycles dropped on device), "
                      f"{link_gaps} link gaps", flush=True)
            cap_idx += 1
            if args.interval_s > 0:
                next_start += args.interval_s
                if next_start < time.time() - t0:
                    next_start = time.time() - t0
            t = time.time() - t0
            if t >= next_status:
                print(f"STATUS t={t/60:6.1f}min captures={cap_idx} rows={row_idx} "
                      f"batch_gaps={tot_gaps} bad_captures={tot_bad}", flush=True)
                next_status += 60.0
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
    finally:
        f.close()
        print(f"DONE. {cap_idx} captures, {row_idx} rows, batch_gaps={tot_gaps}, bad_captures={tot_bad}\n  -> {out}",
              flush=True)
        if saved_apo is not None:
            st, _ = link.req(setop, struct.pack("<H", saved_apo))
            print(f"restore auto_poweroff_s = {saved_apo} [{a.STATUS.get(st, st)}]", flush=True)
        if args.restore:
            st, _ = link.req(a.OP_CMD_SIGNAL_ANALYSIS, bytes([1]))
            print("restart displacement demod ->", st, flush=True)
        link.ser.close()


if __name__ == "__main__":
    main()
