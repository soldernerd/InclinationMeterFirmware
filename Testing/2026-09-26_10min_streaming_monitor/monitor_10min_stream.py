import csv
import struct
import sys
import time

sys.path.insert(0, r"J:\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM6"
DURATION_S = 600
SUB_INTERVAL_MS = 50            # API2_MEASUREMENT_MIN_INTERVAL_MS floor -- ~matches the ~49.2ms batch period
SLOW_INTERVAL_S = 10.0
OUT_CSV = r"C:\Users\lfaes\AppData\Local\Temp\claude\J--OneDrive-EmbeddedSystems-InclinationMeterFirmware\5d47c327-9528-4f27-a1e4-7d3065fb5850\scratchpad\monitor_10min_stream.csv"


class Session:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.02)
        self.ser.reset_input_buffer()
        self.reasm = a.Reassembler()
        self.pending = []

    def _pump(self):
        chunk = self.ser.read(512)
        if chunk:
            self.pending.extend(self.reasm.feed(chunk))

    def request(self, op, payload=b"", timeout=1.5):
        self.ser.write(a.build(op, payload))
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump()
            for i, (op2, st, data) in enumerate(self.pending):
                if op2 == op:
                    return self.pending.pop(i)[1:]
            time.sleep(0.005)
        return (None, None)

    def drain_stream(self, push_op):
        """Pull out every pending frame matching push_op; returns a list of
        (issue_seq, payload_bytes)."""
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

stream_rows = []      # (t_s, issue_seq, delta1_raw, residual1, delta2_raw, residual2)
slow_rows = []         # (t_s, temp_cdeg, clip, ampfault, indrop, outdrop, degen, disp_ok)

t0 = time.time()
last_slow = -SLOW_INTERVAL_S
last_report = 0
prev_seq = None
seq_gaps = 0

while (time.time() - t0) < DURATION_S:
    sess._pump()
    for issue_seq, payload in sess.drain_stream(push_op):
        t = time.time() - t0
        dec = a.decode_topic_raw_displacement(payload)
        if dec is None:
            continue
        stream_rows.append((round(t, 3), issue_seq, dec["delta1_mm_raw"], dec["residual1"],
                             dec["delta2_mm_raw"], dec["residual2"]))
        if prev_seq is not None and ((issue_seq - prev_seq) & 0xFF) != 1:
            seq_gaps += 1
        prev_seq = issue_seq

    t = time.time() - t0
    if t - last_slow >= SLOW_INTERVAL_S:
        st, td = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
        temp = struct.unpack("<h", td[:2])[0] if st == 0 and td and len(td) >= 2 else None
        st, dd = sess.request(a.OP_RAW_DISPLACEMENT_DIAG)
        diag = a.decode_displacement_diag(dd) if st == 0 and dd else None
        slow_rows.append((round(t, 1), temp,
                           diag["clip_count"] if diag else None,
                           diag["amplitude_fault_count"] if diag else None,
                           diag["input_drop"] if diag else None,
                           diag["output_drop"] if diag else None,
                           diag["degenerate"] if diag else None,
                           diag["disp_ok"] if diag else None))
        last_slow = t

    if t - last_report >= 60:
        print(f"  t={t:6.1f}s  stream_samples={len(stream_rows)}  seq_gaps={seq_gaps}", flush=True)
        last_report = t

    time.sleep(0.01)

st, _ = sess.request(a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_RAW_DISPLACEMENT))
print("UNSUBSCRIBE ->", st, flush=True)

with open(OUT_CSV, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["t_s", "issue_seq", "delta1_mm_raw", "residual1", "delta2_mm_raw", "residual2"])
    w.writerows(stream_rows)

slow_csv = OUT_CSV.replace(".csv", "_slow.csv")
with open(slow_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["t_s", "onboard_temp_cdeg", "clip_count", "amplitude_fault_count",
                "input_drop", "output_drop", "degenerate", "disp_ok"])
    w.writerows(slow_rows)

print(f"Done. {len(stream_rows)} stream samples ({seq_gaps} seq gaps), "
      f"{len(slow_rows)} slow samples.", flush=True)
print("wrote", OUT_CSV, "and", slow_csv, flush=True)
sess.ser.close()
