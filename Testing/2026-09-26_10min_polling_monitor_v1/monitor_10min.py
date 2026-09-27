import csv
import struct
import sys
import time

sys.path.insert(0, r"J:\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM6"
DURATION_S = 600
SAMPLE_INTERVAL_S = 0.4
SLOW_INTERVAL_S = 10.0   # temp/battery, changes slowly, don't need every sample
OUT_CSV = r"C:\Users\lfaes\AppData\Local\Temp\claude\J--OneDrive-EmbeddedSystems-InclinationMeterFirmware\5d47c327-9528-4f27-a1e4-7d3065fb5850\scratchpad\monitor_10min.csv"

ser = serial.Serial(PORT, 115200, timeout=0.3)
ser.reset_input_buffer()
reasm = a.Reassembler()
pending = []


def request(op, payload=b"", timeout=1.5):
    ser.write(a.build(op, payload))
    deadline = time.time() + timeout
    while True:
        if pending:
            return pending.pop(0)[1:]
        if time.time() >= deadline:
            return (None, None)
        chunk = ser.read(64)
        if chunk:
            pending.extend(reasm.feed(chunk))


def get_float(cat, res):
    st, d = request(a.opcode(a.GET, cat, res))
    if st == 0 and d and len(d) >= 4:
        return struct.unpack("<f", d[:4])[0]
    return None


def get_diag():
    st, d = request(a.OP_RAW_DISPLACEMENT_DIAG)
    if st == 0 and d and len(d) >= 13:
        return a.decode_displacement_diag(d)
    return None


def get_temp_cdeg():
    st, d = request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
    if st == 0 and d and len(d) >= 2:
        return struct.unpack("<h", d[:2])[0]
    return None


def get_device_state():
    st, d = request(a.OP_SYS_DEVICE_STATE)
    if st == 0 and d:
        return a.decode_device_state(d)
    return None


print("Connecting", PORT, "...", flush=True)
st, data = request(a.OP_SYS_IDENTITY)
print("IDENTITY", st, a.decode_identity(data) if st == 0 else data, flush=True)

fieldnames = ["t_s", "delta1_mm", "residual1", "delta2_mm", "residual2",
              "clip_count", "amplitude_fault_count", "input_drop", "output_drop",
              "degenerate", "disp_ok", "onboard_temp_cdeg", "device_state"]

rows = []
t0 = time.time()
last_slow = 0.0
last_temp = None
last_state = None

while (time.time() - t0) < DURATION_S:
    t = time.time() - t0
    d1 = get_float(a.CAT_MEAS, a.MEAS_DISP1_DELTA_MM)
    r1 = get_float(a.CAT_MEAS, a.MEAS_DISP1_RESIDUAL)
    d2 = get_float(a.CAT_MEAS, a.MEAS_DISP2_DELTA_MM)
    r2 = get_float(a.CAT_MEAS, a.MEAS_DISP2_RESIDUAL)
    diag = get_diag()

    if t - last_slow >= SLOW_INTERVAL_S:
        last_temp = get_temp_cdeg()
        last_state = get_device_state()
        last_slow = t

    rows.append({
        "t_s": round(t, 2),
        "delta1_mm": d1, "residual1": r1,
        "delta2_mm": d2, "residual2": r2,
        "clip_count": diag["clip_count"] if diag else None,
        "amplitude_fault_count": diag["amplitude_fault_count"] if diag else None,
        "input_drop": diag["input_drop"] if diag else None,
        "output_drop": diag["output_drop"] if diag else None,
        "degenerate": diag["degenerate"] if diag else None,
        "disp_ok": diag["disp_ok"] if diag else None,
        "onboard_temp_cdeg": last_temp,
        "device_state": str(last_state) if last_state else None,
    })

    elapsed_this_sample = (time.time() - t0) - t
    remaining = SAMPLE_INTERVAL_S - elapsed_this_sample
    if remaining > 0:
        time.sleep(remaining)

    if len(rows) % 150 == 0:   # ~every 60s at the nominal 0.4s interval
        print(f"  t={t:6.1f}s  d1={d1}  d2={d2}  clip={diag['clip_count'] if diag else '?'}", flush=True)

with open(OUT_CSV, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fieldnames)
    w.writeheader()
    w.writerows(rows)

ser.close()
print(f"Done. {len(rows)} samples written to {OUT_CSV}", flush=True)
