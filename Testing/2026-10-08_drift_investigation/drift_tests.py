"""Unattended drift-investigation test runner (fw >= 0.10.76).

Subscribes to the gapless phasor stream (API Topic 0x05/0x05, every 64-cycle batch) and runs a PLAN of steps while
logging every batch together with the current test state. A step can change, via two investigation commands added
in fw 0.10.76:
  * the excitation: AD9833 on/off and phase (Commands 0x0A; phase code 0..4095 = 0..360 deg, 2048 = 180 deg), also
    chopped (alternating phase and phase+180 deg every `chop` seconds);
  * the ADS131M04 input multiplexer per ADC channel (Commands 0x0B; ch0 = S2, ch1 = B, ch2 = A, ch3 = S1):
    0 normal, 1 inputs shorted, 2/3 DC test signal.
Why: the S1 reading ramps ~+2 um/m per hour, in phase only, S2 much less. These tests tell excitation-proportional
(sensor / leakage) from additive (interference, ADC) terms and the ADC channel from what is upstream of its pins.

One CSV row per batch:
  index, t_s, seq, cycles, gap_cycles, frame_gap, first_batch, iB..qS2, step, exc_on, phase, mux,
  onboard_temp_cdeg, soc_pct, charging, usb, env_temp_cdeg, env_press_pa, env_humid_cpct
  (first_batch = 1 after any discontinuity; the state columns are the state APPLIED at receipt -- discard ~3 s
  after every change, the stream has ~0.5 s of latency.) Slow fields are polled every --poll-s seconds.
The console log (events, STATUS every minute) goes to stdout; redirect it to a file.

On exit (normal, Ctrl-C, error) it restores excitation ON / phase 0 / all channels normal. A board reset (stall)
is recovered automatically (re-subscribe and re-apply the current state). Aborts (restoring) if the state of charge
falls to --abort-soc while not charging. Needs auto power-off disabled on the board (auto_poweroff_s = 0).

python drift_tests.py --plan quick            # ~12 min functional test
python drift_tests.py --plan main             # the full sequence
python drift_tests.py --plan quick --dry-run  # print the schedule only
python drift_tests.py --plan freq             # static response at n = 10, 12, 9, 16 (excitation fs/n), fw >= 0.10.91
python drift_tests.py --plan freqalt          # n = 8 / 10 interleaved in 10 min blocks for 4 h (freqalt12: 8 / 12)
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

F0 = 20833.3333 / 8.0
BATCH_CYCLES = 64
COLUMNS = ["index", "t_s", "seq", "cycles", "gap_cycles", "frame_gap", "first_batch",
           "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2",
           "step", "exc_on", "phase", "mux", "n",
           "onboard_temp_cdeg", "soc_pct", "charging", "usb", "env_temp_cdeg", "env_press_pa", "env_humid_cpct"]

OP_SUB = a.opcode(a.SUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASOR_STREAM)
OP_UNSUB = a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASOR_STREAM)
OP_EXC = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0A)
OP_MUX = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0B)
OP_NFREQ = a.opcode(a.EXECUTE, a.CAT_COMMANDS, 0x0C)   # fw 0.10.91: excitation frequency = fs / n (n = samples per cycle, 4..16)
OP_STATUS = a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_STATUS)
OP_TEMP = a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP)
OP_ENV_T = a.opcode(a.GET, a.CAT_MEAS, a.MEAS_BME280_TEMP)
OP_ENV_P = a.opcode(a.GET, a.CAT_MEAS, a.MEAS_BME280_PRESS)
OP_ENV_H = a.opcode(a.GET, a.CAT_MEAS, a.MEAS_BME280_HUMID)
NORMAL = (0, 0, 0, 0)


def decode_entry(d):
    """One stream entry (34 B): 8 x float32 LE (iB qB iA qA iS1 qS1 iS2 qS2) + u16 LE cycle seq. Local, so the runner does not
    depend on how apiv2.py names its decoder (master renamed it)."""
    if len(d) < 34:
        return None
    iB, qB, iA, qA, iS1, qS1, iS2, qS2, seq = struct.unpack("<8fH", d[:34])
    return dict(iB=iB, qB=qB, iA=iA, qA=qA, iS1=iS1, qS1=qS1, iS2=iS2, qS2=qS2, seq=seq)


# ------------------------------------------------------------------ plans
def step(name, dur, exc=1, phase=0, mux=NORMAL, chop=0, n=8):
    return dict(name=name, dur=dur, exc=exc, phase=phase, mux=tuple(mux), chop=chop, n=n)

def plan_quick():
    p = [step("baseline", 90)]
    for rep in range(2):
        for ph, nm in ((0, "ph000"), (2048, "ph180")):
            p.append(step(nm, 45, phase=ph))
    for ph, nm in ((1024, "ph090"), (3072, "ph270"), (0, "ph000")):
        p.append(step(nm, 45, phase=ph))
    p.append(step("short_ch3_S1", 90, mux=(0, 0, 0, 1)))
    p.append(step("short_ch0_S2", 90, mux=(1, 0, 0, 0)))
    p.append(step("excitation_off", 120, exc=0))
    p.append(step("baseline_after", 90))
    return p

def plan_phase_reversal(reps=4, dwell=60):
    p = [step("baseline", 300)]
    for rep in range(reps):
        for ph, nm in ((0, "ph000"), (2048, "ph180")):
            p.append(step(nm, dwell, phase=ph))
    for rep in range(2):
        for ph, nm in ((1024, "ph090"), (3072, "ph270")):
            p.append(step(nm, dwell, phase=ph))
    p.append(step("ph000_end", 120))
    return p

def plan_main():
    p = plan_phase_reversal()
    p.append(step("short_ch0_ch3", 7200, mux=(1, 0, 0, 1)))      # S2 and S1 ADC channels shorted, excitation on
    p.append(step("baseline_2", 1200))
    p.append(step("excitation_off", 10800, exc=0))               # no excitation for 3 h
    p.append(step("baseline_3_after_off", 3600))
    p.append(step("chop_30s", 21600, chop=30))                   # 0/180 deg every 30 s for 6 h
    p.append(step("baseline_4", 600))
    return p

def plan_freq_static():
    """Static response at several excitation frequencies (fs / n): 3 min each, phase 0, plus a 0/180 reversal at each."""
    p = [step("n08_start", 180, n=8)]
    for n in (10, 12, 9, 16, 8):
        p.append(step(f"n{n:02d}", 180, n=n))
        p.append(step(f"n{n:02d}_ph180", 60, phase=2048, n=n))
        p.append(step(f"n{n:02d}_ph000", 60, phase=0, n=n))
    return p

def plan_freq_alt(hours=4.0, block_s=600, n_alt=10):
    """Interleaved n = 8 / n_alt blocks (block_s each) so both frequencies see the same slow drift."""
    p = [step("n08_start", 300, n=8)]
    for i in range(int(hours * 3600 // block_s)):
        n = n_alt if i % 2 == 0 else 8
        p.append(step(f"alt_n{n:02d}_{i:02d}", block_s, n=n))
    p.append(step("n08_end", 300, n=8))
    return p

PLANS = {"freq": plan_freq_static, "freqalt": plan_freq_alt, "freqalt12": lambda: plan_freq_alt(4.0, 600, 12), "freqalt16": lambda: plan_freq_alt(4.0, 600, 16),
         "quick": plan_quick, "phase": plan_phase_reversal, "main": plan_main,
         "short": lambda: [step("baseline", 600), step("short_ch0_ch3", 7200, mux=(1, 0, 0, 1)), step("baseline_after", 1200)],
         "off": lambda: [step("baseline", 600), step("excitation_off", 10800, exc=0), step("baseline_after", 3600)],
         "chop": lambda: [step("baseline", 600), step("chop_30s", 21600, chop=30), step("baseline_after", 600)]}

def desired(st, tin):
    phase = st["phase"]
    if st["chop"]:
        phase = (phase + (2048 if int(tin // st["chop"]) % 2 else 0)) & 0xFFF
    return (st["exc"], phase, st["mux"], st["n"])


# ------------------------------------------------------------------ helpers
def find_port():
    import serial.tools.list_ports as lp
    for p in lp.comports():
        if "STLink" in (p.description or "") or "ST-Link" in (p.description or ""):
            return p.device
    sys.exit("no ST-Link virtual COM port found; pass --port COMx")

def log(msg):
    print(f"{datetime.now().strftime('%H:%M:%S')} {msg}", flush=True)

def request(ser, reasm, op, payload=b"", timeout=2.0):
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
    ap.add_argument("--plan", default="quick", choices=sorted(PLANS))
    ap.add_argument("--port", default="auto")
    ap.add_argument("--out", default=None)
    ap.add_argument("--poll-s", type=float, default=30.0)
    ap.add_argument("--stall-s", type=float, default=10.0)
    ap.add_argument("--abort-soc", type=int, default=12)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    plan = PLANS[args.plan]()
    total = sum(s["dur"] for s in plan)
    print(f"plan '{args.plan}': {len(plan)} steps, {total/60:.0f} min ({total/3600:.2f} h)")
    for s in plan:
        print(f"  {s['name']:24s} {s['dur']:6d} s  exc={s['exc']} phase={s['phase']:4d} mux={s['mux']} chop={s['chop']}")
    if args.dry_run:
        applied = None; n = 0
        for s in plan:
            for tin in range(0, s["dur"]):
                d = desired(s, tin)
                if d != applied:
                    n += 1
                    if n <= 40 or tin == 0:
                        print(f"    t+{tin:5d}s in {s['name']}: apply exc={d[0]} phase={d[1]} mux={d[2]} n={d[3]}")
                    applied = d
        print(f"  {n} state changes in total")
        return

    import serial
    out = args.out or os.path.join(HERE, "data", datetime.now().strftime(f"drift_{args.plan}_%Y%m%d_%H%M%S.csv"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    port = find_port() if args.port == "auto" else args.port
    ser = serial.Serial(port, 115200, timeout=0.3)
    ser.reset_input_buffer()
    reasm = a.Reassembler()
    st, d = request(ser, reasm, a.OP_SYS_IDENTITY)
    if st != 0:
        sys.exit("IDENTITY failed")
    ident = a.decode_identity(d)
    log(f"IDENTITY: {ident}")
    st, d = request(ser, reasm, a.opcode(a.GET, a.CAT_SETTINGS, a.SET_AUTO_POWEROFF_S))
    if st == 0 and d and struct.unpack("<H", d[:2])[0] != 0:
        log("WARNING: auto_poweroff_s != 0 -- the board may power off during the plan")
    f = open(out, "w", newline="")
    w = csv.writer(f)
    w.writerow(COLUMNS)
    log(f"writing {out}")

    st, _ = request(ser, reasm, OP_SUB, a.build_interval(50))
    if st != 0:
        sys.exit(f"SUBSCRIBE refused: {a.STATUS.get(st, st)}")
    applied = None                      # (exc, phase, mux, n) as applied to the board; None = unknown
    cur_exc, cur_phase, cur_mux, cur_n = 1, 0, NORMAL, 8
    slow = dict(temp=None, soc=None, chg=None, usb=None, et=None, ep=None, eh=None)
    rows = 0; tot_gap = 0; tot_bad = 0; recoveries = 0
    prev_seq = prev_frame = None; cycles = 0
    t0 = time.time(); next_poll = 0.0; next_status = 60.0; last_row_t = time.time()
    step_idx = 0; step_t0 = t0
    cur_step = plan[0]
    log(f"STEP 1/{len(plan)}: {cur_step['name']} ({cur_step['dur']} s)")
    aborted = False

    def send_state(d):
        nonlocal applied, cur_exc, cur_phase, cur_mux, cur_n
        exc, phase, mux, nsel = d
        if applied is None or applied[3] != nsel:
            ser.write(a.build(OP_NFREQ, bytes([nsel])))
            time.sleep(1.0)             # frequency word + demod switch + measurement restart
        if applied is None or (applied[0], applied[1]) != (exc, phase):
            ser.write(a.build(OP_EXC, struct.pack("<BH", exc, phase)))
        base = applied[2] if applied is not None else (None,) * 4
        by_val = {}
        for ch in range(4):
            if mux[ch] != base[ch]:
                by_val.setdefault(mux[ch], 0)
                by_val[mux[ch]] |= (1 << ch)
        for val, mask in by_val.items():
            ser.write(a.build(OP_MUX, bytes([mask, val])))
            time.sleep(0.4)             # the mux command restarts the measurement
        applied = d
        cur_exc, cur_phase, cur_mux, cur_n = exc, phase, mux, nsel

    try:
        while True:
            now = time.time()
            tin = now - step_t0
            if tin >= cur_step["dur"]:
                step_idx += 1
                if step_idx >= len(plan):
                    break
                cur_step = plan[step_idx]; step_t0 = now; tin = 0.0
                log(f"STEP {step_idx+1}/{len(plan)}: {cur_step['name']} ({cur_step['dur']} s)")
            d = desired(cur_step, tin)
            if d != applied:
                log(f"  apply exc={d[0]} phase={d[1]} mux={d[2]} n={d[3]}") if not cur_step["chop"] or applied is None or applied[2] != d[2] or applied[0] != d[0] or applied[3] != d[3] else None
                send_state(d)
            if now - t0 >= next_poll:
                for op in (OP_STATUS, OP_TEMP, OP_ENV_T, OP_ENV_P, OP_ENV_H):
                    ser.write(a.build(op))
                next_poll = (now - t0) + args.poll_s
            if args.stall_s > 0 and time.time() - last_row_t > args.stall_s:
                recoveries += 1
                log(f"  STALL: no batch for {time.time() - last_row_t:.0f} s -> re-subscribing and re-applying the state (#{recoveries})")
                ser.write(a.build(OP_SUB, a.build_interval(50)))
                applied = None
                prev_seq = prev_frame = None
                last_row_t = time.time()
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            for op, status, data in reasm.feed(chunk):
                if op is None or status is None:
                    tot_bad += 1
                    continue
                if op == OP_STATUS and status == 0 and data:
                    s = a.decode_topic_status(data)
                    if s:
                        slow["soc"], slow["chg"], slow["usb"] = s["soc_pct"], int(s["charging"]), int(s["usb"])
                        if slow["soc"] <= args.abort_soc and not slow["chg"]:
                            log(f"  ABORT: state of charge {slow['soc']}% <= {args.abort_soc}% and not charging")
                            aborted = True
                    continue
                if op == OP_TEMP and status == 0 and data and len(data) >= 2:
                    slow["temp"] = struct.unpack("<h", data[:2])[0]; continue
                if op == OP_ENV_T and status == 0 and data and len(data) >= 2:
                    slow["et"] = struct.unpack("<h", data[:2])[0]; continue
                if op == OP_ENV_P and status == 0 and data and len(data) >= 4:
                    slow["ep"] = struct.unpack("<I", data[:4])[0]; continue
                if op == OP_ENV_H and status == 0 and data and len(data) >= 2:
                    slow["eh"] = struct.unpack("<H", data[:2])[0]; continue
                if op in (OP_EXC, OP_MUX):
                    if status != 0:
                        log(f"  command 0x{op:04X} refused: {a.STATUS.get(status, status)}")
                    continue
                if op == OP_SUB and status != 0:
                    log(f"  SUBSCRIBE refused: {a.STATUS.get(status, status)}")
                    continue
                if op != OP_SUB or status != 0 or len(data) < 2 + 34:
                    continue
                frame_seq = data[0]
                e = decode_entry(data[2:2 + 34])
                if e is None:
                    tot_bad += 1
                    continue
                frame_gap = 0 if prev_frame is None else ((frame_seq - prev_frame - 1) & 0xFF)
                prev_frame = frame_seq
                if prev_seq is None:
                    gap, first = 0, 1
                    cycles = e["seq"]
                else:
                    stp = (e["seq"] - prev_seq) & 0xFFFF
                    gap = stp - BATCH_CYCLES
                    first = 1 if gap != 0 else 0           # any discontinuity (drops, a mux restart, a reset)
                    cycles += stp
                prev_seq = e["seq"]
                if gap:
                    tot_gap += 1
                mux_s = "".join(str(m) for m in cur_mux)
                w.writerow([rows, f"{time.time() - t0:.4f}", e["seq"], cycles, gap, frame_gap, first,
                            repr(e["iB"]), repr(e["qB"]), repr(e["iA"]), repr(e["qA"]),
                            repr(e["iS1"]), repr(e["qS1"]), repr(e["iS2"]), repr(e["qS2"]),
                            cur_step["name"], cur_exc, cur_phase, mux_s, cur_n,
                            "" if slow["temp"] is None else slow["temp"], "" if slow["soc"] is None else slow["soc"],
                            "" if slow["chg"] is None else slow["chg"], "" if slow["usb"] is None else slow["usb"],
                            "" if slow["et"] is None else slow["et"], "" if slow["ep"] is None else slow["ep"],
                            "" if slow["eh"] is None else slow["eh"]])
                rows += 1
                last_row_t = time.time()
                if rows % 400 == 0:
                    f.flush()
            t = time.time() - t0
            if t >= next_status:
                log(f"STATUS t={t/60:6.1f}min step={cur_step['name']} rows={rows} ({rows/t:.2f}/s) gap_events={tot_gap} "
                    f"bad={tot_bad} stalls={recoveries} soc={slow['soc']} chg={slow['chg']} "
                    f"env={slow['et']}cdeg/{slow['ep']}Pa/{slow['eh']}c%RH")
                next_status += 60.0
            if aborted:
                break
    except KeyboardInterrupt:
        log("interrupted")
    finally:
        log("restoring: excitation ON, phase 0, n = 8 (2604 Hz), all ADC channels normal")
        try:
            ser.write(a.build(OP_EXC, struct.pack("<BH", 1, 0)))
            time.sleep(0.2)
            ser.write(a.build(OP_NFREQ, bytes([8])))
            time.sleep(1.0)
            ser.write(a.build(OP_MUX, bytes([0x0F, 0])))
            time.sleep(0.6)
        except Exception as ex:
            log(f"restore failed: {ex!r}")
        f.close()
        ser.write(a.build(OP_UNSUB))
        time.sleep(0.5)
        ser.reset_input_buffer()
        log(f"DONE. {rows} rows, gap_events={tot_gap}, bad={tot_bad}, stalls={recoveries}, aborted={aborted} -> {out}")
        ser.close()


if __name__ == "__main__":
    main()
