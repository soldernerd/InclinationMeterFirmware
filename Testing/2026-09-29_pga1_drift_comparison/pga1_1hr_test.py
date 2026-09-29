"""
1-hour drift/noise comparison at PGA=1 (S1/S2), identical method to
Testing/2026-09-27_24hr_granite_plate_test/ (same flat-CSV layout, same
fields: raw displacement + phasors + full error-term set), run specifically
to check whether the 24h test's temperature-correlated drift survives with
the ADC's own PGA=16 stage taken out of the picture (fw 0.10.58,
Drivers_App/drv_ads131m04.c's GAIN1_REG_VALUE temporarily 0x0000; software
disp_s1/s2_gain_milli reverted to 10000 to match).

Not a resumable/pausable script like the 24h one -- a single uninterrupted
hour, no pause/resume support needed.
"""
import csv
import time

import sys
sys.path.insert(0, r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM5"
DURATION_S = 3600            # 1 hour
RAW_SUB_MS = 50               # match the 24h test's 20Hz raw-displacement cadence
PHASOR_SUB_MS = 50
SLOW_POLL_S = 30.0
STATUS_INTERVAL_S = 600.0     # report every 10 minutes, as asked
OUT_CSV = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-29_pga1_drift_comparison\data\pga1_1hr.csv"


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


FIELDS = [
    "index", "t_s",
    "delta1_mm_raw", "residual1", "delta2_mm_raw", "residual2",
    "quality1_ok", "quality2_ok", "delta_diff_mm_raw", "quality_diff_ok",
    "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2", "phasor_age_s",
    "onboard_temp_cdeg", "battery_mV", "soc_pct", "usb", "charging", "force_charging",
    "clip_count", "amplitude_fault_count", "input_drop", "output_drop",
    "max_update_gap_ms", "max_gap_at_uptime_ms", "gap_over_threshold_count", "degenerate",
    "adc_frame_deficit", "adc_framing_err", "adc_crc_err", "adc_ring_overflow", "adc_backlog",
    "rms_B_mV", "rms_A_mV", "rms_S1_mV", "rms_S2_mV", "phase_S1_deg", "phase_S2_deg",
    "theoretical_tilt1_mm_per_m", "theoretical_tilt2_mm_per_m",
    "event",
]

sess = Session(PORT)
st, data = sess.request(a.OP_SYS_IDENTITY)
print("IDENTITY:", st, a.decode_identity(data) if st == 0 else data, flush=True)
if st != 0:
    sys.exit("IDENTITY failed -- wrong port or device not responding")

st, dok = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_DISP_OK))
disp_ok = st == 0 and dok and dok[0]
print("disp_ok at launch:", disp_ok, flush=True)
if not disp_ok:
    sys.exit("displacement demod is not running")

st, dd = sess.request(a.OP_RAW_DISPLACEMENT_DIAG)
baseline_diag = a.decode_displacement_diag(dd) if st == 0 and dd else None
print("baseline displacement diag:", baseline_diag, flush=True)

st, gd = sess.request(a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_SIGNAL_DIAG))
sig0 = a.decode_topic_signal_diag(gd) if st == 0 and gd else None
print("baseline signal diag (PGA=1 scale):", sig0, flush=True)

push_raw_op = a.opcode(a.SUBSCRIBE, a.CAT_TOPICS, a.TOPIC_RAW_DISPLACEMENT)
st, _ = sess.request(push_raw_op, a.build_interval(RAW_SUB_MS))
print(f"SUBSCRIBE raw_displacement @ {RAW_SUB_MS}ms ->", st, flush=True)
if st != 0:
    sys.exit("subscribe raw_displacement failed")

push_phasor_op = a.opcode(a.SUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASORS)
st, _ = sess.request(push_phasor_op, a.build_interval(PHASOR_SUB_MS))
print(f"SUBSCRIBE phasors @ {PHASOR_SUB_MS}ms ->", st, flush=True)
if st != 0:
    sys.exit("subscribe phasors failed")

f = open(OUT_CSV, "w", newline="")
w = csv.writer(f)
w.writerow(FIELDS)

t0 = time.time()
idx = 0
last_slow = -SLOW_POLL_S
next_status = STATUS_INTERVAL_S
prev_raw_seq = None
prev_phasor_seq = None
raw_seq_gaps = 0
phasor_seq_gaps = 0

latest_phasor = dict(iB=None, qB=None, iA=None, qA=None, iS1=None, qS1=None, iS2=None, qS2=None)
last_phasor_t = None
slow = dict(onboard_temp_cdeg=None, battery_mV=None, soc_pct=None, usb=None, charging=None,
            force_charging=None, clip_count=None, amplitude_fault_count=None, input_drop=None,
            output_drop=None, max_update_gap_ms=None, max_gap_at_uptime_ms=None,
            gap_over_threshold_count=None, degenerate=None, adc_frame_deficit=None,
            adc_framing_err=None, adc_crc_err=None, adc_ring_overflow=None, adc_backlog=None,
            rms_B_mV=None, rms_A_mV=None, rms_S1_mV=None, rms_S2_mV=None,
            phase_S1_deg=None, phase_S2_deg=None,
            theoretical_tilt1_mm_per_m=None, theoretical_tilt2_mm_per_m=None)
prev_usb = None
prev_charging = None
pending_event = "test_start_pga1"

window = []

print(f"STATUS t=0.0min n=0 -- 1h PGA=1 comparison test starting, PORT={PORT}", flush=True)

try:
    while (time.time() - t0) < DURATION_S:
        sess._pump()

        for issue_seq, payload in sess.drain_stream(push_phasor_op):
            dec = a.decode_topic_phasors(payload)
            if dec is None:
                continue
            latest_phasor = dec
            last_phasor_t = time.time() - t0
            if prev_phasor_seq is not None and ((issue_seq - prev_phasor_seq) & 0xFF) != 1:
                phasor_seq_gaps += 1
            prev_phasor_seq = issue_seq

        for issue_seq, payload in sess.drain_stream(push_raw_op):
            dec = a.decode_topic_raw_displacement(payload)
            if dec is None:
                continue
            t = time.time() - t0
            if prev_raw_seq is not None and ((issue_seq - prev_raw_seq) & 0xFF) != 1:
                raw_seq_gaps += 1
            prev_raw_seq = issue_seq

            row = [idx, round(t, 3),
                   dec["delta1_mm_raw"], dec["residual1"], dec["delta2_mm_raw"], dec["residual2"],
                   int(dec["quality1_ok"]), int(dec["quality2_ok"]),
                   dec["delta_diff_mm_raw"], int(dec["quality_diff_ok"]),
                   latest_phasor["iB"], latest_phasor["qB"], latest_phasor["iA"], latest_phasor["qA"],
                   latest_phasor["iS1"], latest_phasor["qS1"], latest_phasor["iS2"], latest_phasor["qS2"],
                   (round(t - last_phasor_t, 3) if last_phasor_t is not None else None),
                   slow["onboard_temp_cdeg"], slow["battery_mV"], slow["soc_pct"],
                   slow["usb"], slow["charging"], slow["force_charging"],
                   slow["clip_count"], slow["amplitude_fault_count"], slow["input_drop"], slow["output_drop"],
                   slow["max_update_gap_ms"], slow["max_gap_at_uptime_ms"], slow["gap_over_threshold_count"],
                   slow["degenerate"],
                   slow["adc_frame_deficit"], slow["adc_framing_err"], slow["adc_crc_err"],
                   slow["adc_ring_overflow"], slow["adc_backlog"],
                   slow["rms_B_mV"], slow["rms_A_mV"], slow["rms_S1_mV"], slow["rms_S2_mV"],
                   slow["phase_S1_deg"], slow["phase_S2_deg"],
                   slow["theoretical_tilt1_mm_per_m"], slow["theoretical_tilt2_mm_per_m"],
                   pending_event]
            w.writerow(row)
            pending_event = ""
            idx += 1
            window.append((t, dec["delta1_mm_raw"], dec["delta2_mm_raw"], dec["delta_diff_mm_raw"]))

        t = time.time() - t0

        if t - last_slow >= SLOW_POLL_S:
            st, td = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
            temp = None
            if st == 0 and td and len(td) >= 2:
                import struct
                temp = struct.unpack("<h", td[:2])[0]

            st, sd = sess.request(a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_STATUS))
            status = a.decode_topic_status(sd) if st == 0 and sd else None

            st, dd = sess.request(a.OP_RAW_DISPLACEMENT_DIAG)
            diag = a.decode_displacement_diag(dd) if st == 0 and dd else None

            st, ad = sess.request(a.OP_RAW_ADC_DIAG)
            adc = a.decode_adc_diag(ad) if st == 0 and ad else None

            st, gd = sess.request(a.opcode(a.GET, a.CAT_TOPICS, a.TOPIC_SIGNAL_DIAG))
            sig = a.decode_topic_signal_diag(gd) if st == 0 and gd else None

            slow["onboard_temp_cdeg"] = temp
            if status:
                slow["battery_mV"] = status["battery_mV"]
                slow["soc_pct"] = status["soc_pct"]
                slow["usb"] = int(status["usb"])
                slow["charging"] = int(status["charging"])
                slow["force_charging"] = int(status["force_charging"])
                if prev_usb is not None and status["usb"] != prev_usb:
                    pending_event = "USB_CONNECTED" if status["usb"] else "USB_DISCONNECTED"
                if prev_charging is not None and status["charging"] != prev_charging:
                    pending_event = (pending_event + "+" if pending_event else "") + \
                        ("CHARGE_START" if status["charging"] else "CHARGE_STOP")
                prev_usb = status["usb"]
                prev_charging = status["charging"]
            if diag:
                slow["clip_count"] = diag["clip_count"]
                slow["amplitude_fault_count"] = diag["amplitude_fault_count"]
                slow["input_drop"] = diag["input_drop"]
                slow["output_drop"] = diag["output_drop"]
                slow["max_update_gap_ms"] = diag["max_update_gap_ms"]
                slow["max_gap_at_uptime_ms"] = diag["max_gap_at_uptime_ms"]
                slow["gap_over_threshold_count"] = diag["gap_over_threshold_count"]
                slow["degenerate"] = diag["degenerate"]
            if adc and "integrity" in adc:
                integ = adc["integrity"]
                slow["adc_frame_deficit"] = integ["frame_deficit"]
                slow["adc_framing_err"] = integ["framing_err"]
                slow["adc_crc_err"] = integ["crc_err"]
                slow["adc_ring_overflow"] = integ["ring_overflow"]
                slow["adc_backlog"] = integ["backlog"]
            if sig:
                slow["rms_B_mV"] = sig["rms_mv"]["B"]
                slow["rms_A_mV"] = sig["rms_mv"]["A"]
                slow["rms_S1_mV"] = sig["rms_mv"]["S1"]
                slow["rms_S2_mV"] = sig["rms_mv"]["S2"]
                slow["phase_S1_deg"] = sig["phase_deg"]["S1"]
                slow["phase_S2_deg"] = sig["phase_deg"]["S2"]
                slow["theoretical_tilt1_mm_per_m"] = sig["theoretical_tilt1_mm_per_m"]
                slow["theoretical_tilt2_mm_per_m"] = sig["theoretical_tilt2_mm_per_m"]
            f.flush()
            last_slow = t
            window[:] = [ww for ww in window if ww[0] >= t - STATUS_INTERVAL_S - 60]

        if t >= next_status:
            recent = [ww for ww in window if ww[0] >= max(0.0, t - STATUS_INTERVAL_S)]

            def stats(vals):
                if not vals:
                    return float("nan"), float("nan")
                m = sum(vals) / len(vals)
                sd = (sum((x - m) ** 2 for x in vals) / len(vals)) ** 0.5
                return m, sd

            d1s = [ww[1] for ww in recent]
            d2s = [ww[2] for ww in recent]
            ddiffs = [ww[3] for ww in recent]
            m1, s1 = stats(d1s)
            m2, s2 = stats(d2s)
            md, sd = stats(ddiffs)
            print(f"STATUS t={t/60:6.1f}min n={idx} raw_seq_gaps={raw_seq_gaps} "
                  f"phasor_seq_gaps={phasor_seq_gaps} "
                  f"temp={(slow['onboard_temp_cdeg']/100 if slow['onboard_temp_cdeg'] is not None else 'NA')}C "
                  f"batt={slow['battery_mV']}mV soc={slow['soc_pct']}% "
                  f"usb={slow['usb']} charging={slow['charging']} "
                  f"S1 mean={m1:.5f} std={s1:.6f} S2 mean={m2:.5f} std={s2:.6f} "
                  f"diff mean={md:.5f} std={sd:.6f} "
                  f"rmsS1={slow['rms_S1_mV']} rmsS2={slow['rms_S2_mV']} "
                  f"clip={slow['clip_count']} ampfault={slow['amplitude_fault_count']} "
                  f"in_drop={slow['input_drop']} out_drop={slow['output_drop']} "
                  f"max_gap={slow['max_update_gap_ms']}ms gaps>thr={slow['gap_over_threshold_count']}",
                  flush=True)
            next_status += STATUS_INTERVAL_S

        time.sleep(0.01)

except Exception as e:
    pending_event = f"SCRIPT_ERROR:{e}"
    print(f"ERROR at t={time.time()-t0:.1f}s: {e}", flush=True)
    raise
finally:
    try:
        sess.request(a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_RAW_DISPLACEMENT))
        sess.request(a.opcode(a.UNSUBSCRIBE, a.CAT_TOPICS, a.TOPIC_PHASORS))
    except Exception:
        pass
    f.close()
    print(f"DONE. {idx} rows, raw_seq_gaps={raw_seq_gaps}, phasor_seq_gaps={phasor_seq_gaps}, "
          f"wrote {OUT_CSV}", flush=True)
    sess.ser.close()
