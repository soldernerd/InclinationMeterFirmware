import csv
import struct
import sys
import time

sys.path.insert(0, r"J:\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM6"
DURATION_S = 7200          # 2 hours
CHARGE_ON_AT_S = 3600      # force charging on at the halfway point
SUB_INTERVAL_MS = 50
SLOW_INTERVAL_S = 30.0
STATUS_INTERVAL_S = 300.0  # print a summary line every 5 min (Monitor watches for these)
BASE = r"C:\Users\lfaes\AppData\Local\Temp\claude\J--OneDrive-EmbeddedSystems-InclinationMeterFirmware\5d47c327-9528-4f27-a1e4-7d3065fb5850\scratchpad"
OUT_CSV = BASE + r"\long_drift_stream.csv"
SLOW_CSV = BASE + r"\long_drift_slow.csv"


class Session:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.02)
        self.ser.reset_input_buffer()
        self.reasm = a.Reassembler()
        self.pending = []

    def _pump(self):
        try:
            chunk = self.ser.read(512)
        except Exception:
            chunk = b""
        if chunk:
            self.pending.extend(self.reasm.feed(chunk))

    def request(self, op, payload=b"", timeout=2.0):
        try:
            self.ser.write(a.build(op, payload))
        except Exception:
            return (None, None)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump()
            for i, (op2, st, data) in enumerate(self.pending):
                if op2 == op:
                    return self.pending.pop(i)[1:]
            time.sleep(0.005)
        return (None, None)

    def drain_stream(self, push_op):
        out = []
        still = []
        for (op2, st, data) in self.pending:
            if op2 == push_op and st == 0 and data and len(data) >= 2:
                out.append((data[0], data[2:]))
            else:
                still.append((op2, st, data))
        self.pending = still
        return out


sess = Session(PORT)
st, data = sess.request(a.OP_SYS_IDENTITY)
print("IDENTITY", st, a.decode_identity(data) if st == 0 else data, flush=True)

push_op = a.opcode(a.SUBSCRIBE, a.CAT_TOPICS, a.TOPIC_RAW_DISPLACEMENT)
st, _ = sess.request(push_op, a.build_interval(SUB_INTERVAL_MS))
print(f"SUBSCRIBE raw_displacement @ {SUB_INTERVAL_MS}ms ->", st, flush=True)
if st != 0:
    sys.exit("subscribe failed")

stream_f = open(OUT_CSV, "w", newline="")
stream_w = csv.writer(stream_f)
stream_w.writerow(["t_s", "issue_seq", "delta1_mm_raw", "residual1", "delta2_mm_raw", "residual2",
                    "quality1_ok", "quality2_ok"])

slow_f = open(SLOW_CSV, "w", newline="")
slow_w = csv.writer(slow_f)
slow_w.writerow(["t_s", "onboard_temp_cdeg", "charging", "usb", "battery_mv",
                  "clip_count", "amplitude_fault_count", "input_drop", "output_drop",
                  "max_update_gap_ms", "gap_over_threshold_count", "event"])

t0 = time.time()
last_slow = -SLOW_INTERVAL_S
last_status = -STATUS_INTERVAL_S
prev_seq = None
seq_gaps = 0
n_samples = 0
charge_forced = False

# rolling window for status summaries: (t, d1, d2)
window = []

print(f"STATUS t=0.0min charging=False (natural) -- baseline/warm-up phase begins", flush=True)

while (time.time() - t0) < DURATION_S:
    sess._pump()
    for issue_seq, payload in sess.drain_stream(push_op):
        t = time.time() - t0
        dec = a.decode_topic_raw_displacement(payload)
        if dec is None:
            continue
        stream_w.writerow([round(t, 3), issue_seq, dec["delta1_mm_raw"], dec["residual1"],
                            dec["delta2_mm_raw"], dec["residual2"],
                            int(dec["quality1_ok"]), int(dec["quality2_ok"])])
        n_samples += 1
        window.append((t, dec["delta1_mm_raw"], dec["delta2_mm_raw"]))
        if prev_seq is not None and ((issue_seq - prev_seq) & 0xFF) != 1:
            seq_gaps += 1
        prev_seq = issue_seq

    t = time.time() - t0

    # trigger forced charging at the halfway point
    if (not charge_forced) and t >= CHARGE_ON_AT_S:
        st, _ = sess.request(a.OP_CMD_FORCE_CHARGE)
        charge_forced = True
        print(f"EVENT t={t/60:.1f}min FORCE_CHARGE issued -> status {st}", flush=True)

    if t - last_slow >= SLOW_INTERVAL_S:
        st, td = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
        temp = struct.unpack("<h", td[:2])[0] if st == 0 and td and len(td) >= 2 else None
        st, sd = sess.request(a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_STATUS))
        status = a.decode_topic_status(sd) if st == 0 and sd else None
        st, dd = sess.request(a.OP_RAW_DISPLACEMENT_DIAG)
        diag = a.decode_displacement_diag(dd) if st == 0 and dd else None
        slow_w.writerow([
            round(t, 1), temp,
            status["charging"] if status else None,
            status["usb"] if status else None,
            status["battery_mV"] if status else None,
            diag["clip_count"] if diag else None,
            diag["amplitude_fault_count"] if diag else None,
            diag["input_drop"] if diag else None,
            diag["output_drop"] if diag else None,
            diag["max_update_gap_ms"] if diag else None,
            diag["gap_over_threshold_count"] if diag else None,
            "",
        ])
        slow_f.flush()
        last_slow = t

    if t - last_status >= STATUS_INTERVAL_S:
        stream_f.flush()
        recent = [w for w in window if w[0] >= t - STATUS_INTERVAL_S]
        if recent:
            d1s = [w[1] for w in recent]
            d2s = [w[2] for w in recent]
            mean1 = sum(d1s) / len(d1s)
            mean2 = sum(d2s) / len(d2s)
            std1 = (sum((x - mean1) ** 2 for x in d1s) / len(d1s)) ** 0.5
            std2 = (sum((x - mean2) ** 2 for x in d2s) / len(d2s)) ** 0.5
        else:
            mean1 = mean2 = std1 = std2 = float("nan")
        phase = "CHARGING" if charge_forced else "baseline"
        print(f"STATUS t={t/60:5.1f}min [{phase}] n={n_samples} seq_gaps={seq_gaps} "
              f"temp={temp/100 if temp is not None else 'NA'}C "
              f"S1 mean={mean1:.4f} std={std1:.5f} S2 mean={mean2:.4f} std={std2:.5f}",
              flush=True)
        last_status = t

    time.sleep(0.01)

st, _ = sess.request(a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_RAW_DISPLACEMENT))
print("UNSUBSCRIBE ->", st, flush=True)
stream_f.close()
slow_f.close()
print(f"DONE. {n_samples} stream samples, {seq_gaps} seq gaps, wrote {OUT_CSV} and {SLOW_CSV}", flush=True)
sess.ser.close()
