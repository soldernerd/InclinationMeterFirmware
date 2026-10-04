"""Gapless 64-cycle phasor stream logger (fw >= 0.10.65).

Subscribes to API Topic 0x05 / resource 0x05 ("phasor batch stream"): the firmware
pushes EVERY completed 64-cycle batch (24.6 ms, ~40.7 Hz) with all four phasors
(B, A, S1, S2 as I/Q) the moment it completes -- no 512-batch buffer, no transfer
pause like phasor_capture.py's bulk path.

One flat CSV, one row per batch:
  index, t_s, seq, cycles, gap_cycles, frame_gap, first_batch,
  iB,qB,iA,qA,iS1,qS1,iS2,qS2, onboard_temp_cdeg, soc_pct, charging, usb, label
  * seq        = device cycle counter at the end of the batch (u16, wraps)
  * cycles     = unwrapped cycle count since the stream started (exact device time:
                 t = cycles / 2604.1667 s)
  * gap_cycles = cycles lost since the previous row (0 = contiguous; a multiple of 64)
  * frame_gap  = frames lost on the wire since the previous row (issue_seq jump)
  * first_batch= 1 for the first batch after the ADC starts (stale first sample; drop it)
  * t_s        = host time at receipt, only for orientation (use `cycles` for analysis)
  Slow fields (temp / SoC / charging / usb) are polled every --poll-s seconds.

Charging: the board charges on its own when it needs to. If it has been charging
continuously for --max-charge-h hours (default 3), the script sends Commands 0x09
(charge inhibit) and keeps charging off for the rest of the run; the inhibit is cleared
again when the script exits normally. (It lives in RAM: a reboot also clears it.)

Usage:
  python phasor_stream.py --duration-min 1                     # smoke test
  python phasor_stream.py --duration-min 1440 --label day24h   # a full day
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

F0 = 20833.3333 / 8.0     # cycles per second
BATCH_CYCLES = 64
COLUMNS = ["index", "t_s", "seq", "cycles", "gap_cycles", "frame_gap", "first_batch",
           "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2",
           "onboard_temp_cdeg", "soc_pct", "charging", "usb", "label"]

OP_SUB = a.opcode(a.SUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASOR_STREAM)
OP_UNSUB = a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASOR_STREAM)
OP_TEMP = a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP)
OP_STATUS = a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_STATUS)
OP_INHIBIT = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x09)   # API2_RES_CMD_CHARGE_INHIBIT, payload 0/1


def find_port():
    import serial.tools.list_ports as lp
    for p in lp.comports():
        if "STLink" in (p.description or "") or "ST-Link" in (p.description or ""):
            return p.device
    sys.exit("no ST-Link virtual COM port found; pass --port COMx")


def request(ser, reasm, op, payload=b"", timeout=2.0):
    """Blocking request/response (only used before/after the stream)."""
    ser.write(a.build(op, payload))
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(256)
        for pop, st, data in (reasm.feed(chunk) if chunk else []):
            if pop == op:
                return st, data
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="auto")
    ap.add_argument("--duration-min", type=float, default=1.0)
    ap.add_argument("--poll-s", type=float, default=60.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--label", default="")
    ap.add_argument("--max-charge-h", type=float, default=3.0,
                    help="inhibit charging after this many hours of continuous charging (0 = never)")
    ap.add_argument("--keep-autopoweroff", action="store_true")
    ap.add_argument("--restore", action="store_true", help="restart the displacement demod at the end")
    args = ap.parse_args()

    out = args.out or os.path.join(HERE, "data", datetime.now().strftime("phasor_stream_%Y%m%d_%H%M%S.csv"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    port = find_port() if args.port == "auto" else args.port
    print("port:", port, flush=True)

    ser = serial.Serial(port, 115200, timeout=0.3)
    ser.reset_input_buffer()
    reasm = a.Reassembler()
    st, d = request(ser, reasm, a.OP_SYS_IDENTITY)
    if st != 0:
        sys.exit("IDENTITY failed -- check --port / that the board is awake")
    print("IDENTITY:", a.decode_identity(d), flush=True)

    st, _ = request(ser, reasm, a.OP_CMD_SIGNAL_ANALYSIS, bytes([0]))   # the stream needs the demod stopped
    print("stop displacement demod ->", st, flush=True)

    getop = a.opcode(a.GET, a.CAT_SETTINGS, a.SET_AUTO_POWEROFF_S)
    setop = a.opcode(a.SET, a.CAT_SETTINGS, a.SET_AUTO_POWEROFF_S)
    saved_apo = None
    st, d = request(ser, reasm, getop)
    if st == 0 and d and len(d) >= 2:
        cur = struct.unpack("<H", d[:2])[0]
        if cur != 0 and not args.keep_autopoweroff:
            st2, _ = request(ser, reasm, setop, struct.pack("<H", 0))
            if st2 == 0:
                saved_apo = cur
            print(f"auto_poweroff_s {cur} -> 0 for the run [{a.STATUS.get(st2, st2)}] (restored at the end)", flush=True)

    f = open(out, "w", newline="")
    w = csv.writer(f)
    w.writerow(COLUMNS)
    print(f"writing {out}\n  duration {args.duration_min} min", flush=True)

    st, _ = request(ser, reasm, OP_SUB, a.build_interval(50))
    if st != 0:
        f.close()
        if saved_apo is not None:
            request(ser, reasm, setop, struct.pack("<H", saved_apo))
        sys.exit(f"SUBSCRIBE phasor stream refused: {a.STATUS.get(st, st)}")
    reasm = a.Reassembler()   # drop anything buffered while waiting for the ack
    t0 = time.time()
    t_end = t0 + args.duration_min * 60.0
    next_poll = 0.0
    next_status = 60.0
    temp = soc = chg = usb = None
    rows = bad = tot_gap_batches = tot_frame_gaps = 0
    prev_seq = prev_frame = None
    cycles = 0
    charge_since = None       # host time the current continuous charging stretch began
    inhibited = False

    try:
        while time.time() < t_end:
            now = time.time() - t0
            if now >= next_poll:
                ser.write(a.build(OP_TEMP))
                ser.write(a.build(OP_STATUS))
                next_poll = now + args.poll_s
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            for op, status, data in reasm.feed(chunk):
                if op is None or status is None:
                    bad += 1
                    continue
                if op == OP_TEMP and status == 0 and data and len(data) >= 2:
                    temp = struct.unpack("<h", data[:2])[0]
                    continue
                if op == OP_STATUS and status == 0 and data:
                    s = a.decode_topic_status(data)
                    if s:
                        soc, chg, usb = s["soc_pct"], int(s["charging"]), int(s["usb"])
                        tnow = time.time()
                        if chg and charge_since is None:
                            charge_since = tnow
                            print(f"  charging started at t={(tnow - t0)/60:.1f}min (soc {soc}%)", flush=True)
                        elif not chg and charge_since is not None:
                            print(f"  charging ended after {(tnow - charge_since)/60:.1f}min (soc {soc}%)", flush=True)
                            charge_since = None
                        if (args.max_charge_h > 0 and chg and not inhibited
                                and tnow - charge_since >= args.max_charge_h * 3600.0):
                            ser.write(a.build(OP_INHIBIT, bytes([1])))
                            inhibited = True      # confirmed (or not) by the ack below
                            print(f"  charging for {args.max_charge_h} h: charge INHIBIT sent (soc {soc}%)", flush=True)
                    continue
                if op == OP_INHIBIT:
                    print(f"  charge inhibit ack: {a.STATUS.get(status, status)}", flush=True)
                    if status != 0:
                        inhibited = False         # retry at the next poll
                    continue
                if op != OP_SUB or status != 0 or len(data) < 2 + 34:
                    continue
                frame_seq = data[0]
                e = a.decode_phasor_log_entry(data[2:2 + 34])
                if e is None:
                    bad += 1
                    continue
                frame_gap = 0 if prev_frame is None else ((frame_seq - prev_frame - 1) & 0xFF)
                prev_frame = frame_seq
                if prev_seq is None:
                    gap, first = 0, 1
                    cycles = e["seq"]
                else:
                    step = (e["seq"] - prev_seq) & 0xFFFF
                    gap, first = step - BATCH_CYCLES, 0
                    cycles += step
                prev_seq = e["seq"]
                if gap:
                    tot_gap_batches += 1
                if frame_gap:
                    tot_frame_gaps += 1
                w.writerow([rows, f"{time.time() - t0:.4f}", e["seq"], cycles, gap, frame_gap, first,
                            repr(e["iB"]), repr(e["qB"]), repr(e["iA"]), repr(e["qA"]),
                            repr(e["iS1"]), repr(e["qS1"]), repr(e["iS2"]), repr(e["qS2"]),
                            "" if temp is None else temp, "" if soc is None else soc,
                            "" if chg is None else chg, "" if usb is None else usb, args.label])
                rows += 1
                if rows % 400 == 0:
                    f.flush()
            t = time.time() - t0
            if t >= next_status:
                print(f"STATUS t={t/60:6.1f}min rows={rows} ({rows/t:.2f}/s) gaps={tot_gap_batches} "
                      f"frame_gaps={tot_frame_gaps} bad={bad}", flush=True)
                next_status += 60.0
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
    finally:
        f.close()
        elapsed = time.time() - t0
        ser.write(a.build(OP_UNSUB))
        time.sleep(0.5)
        ser.reset_input_buffer()          # discard queued stream frames + the unsub ack
        reasm = a.Reassembler()
        ideal = F0 / BATCH_CYCLES
        print(f"DONE. {rows} rows in {elapsed:.1f} s ({rows/elapsed:.2f}/s; ideal {ideal:.2f}/s), "
              f"batch_gaps={tot_gap_batches}, frame_gaps={tot_frame_gaps}, bad_frames={bad}\n  -> {out}",
              flush=True)
        if inhibited:
            st, _ = request(ser, reasm, OP_INHIBIT, bytes([0]))
            print(f"charge inhibit cleared [{a.STATUS.get(st, st)}]", flush=True)
        if saved_apo is not None:
            st, _ = request(ser, reasm, setop, struct.pack("<H", saved_apo))
            print(f"restore auto_poweroff_s = {saved_apo} [{a.STATUS.get(st, st)}]", flush=True)
        if args.restore:
            st, _ = request(ser, reasm, a.OP_CMD_SIGNAL_ANALYSIS, bytes([1]))
            print("restart displacement demod ->", st, flush=True)
        ser.close()


if __name__ == "__main__":
    main()
