"""
24-hour unattended monitor on the granite surface plate. Same kind of test as
Testing/2026-09-27_2hr_charging_drift_test/ (continuous streaming subscribe,
periodic slow-channel polling) extended to 24h, plus phasors (Topic 0x5/0x02)
and the full set of acquisition/demod error terms, all merged into ONE flat
CSV per Testing/README.md's "Data format convention for new tests" (index,
timestamp, onboard temp, SoC, charging flag always present; no separate
fast/slow files needing a join).

Row cadence is driven by the raw-displacement subscription (RAW_SUB_MS below).
The phasor subscription and the slow GET-polled fields (temp/status/diag/
signal-diag) update asynchronously and are forward-filled into whichever row
is emitted next. Kept at 50ms/~20Hz (same as the 2h test) at the user's
explicit request -- "keep the cadence at 20hz, we want to see everything and
data volume isn't that great" -- expect roughly 800MB-1GB for the full 24h.

USB stays connected throughout (device charges itself as needed) -- unlike
the 2h test, this run does NOT force charging on/off; it just observes
whatever the device does naturally.
"""

import csv
import struct
import sys
import time

sys.path.insert(0, r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM5"                # confirmed via IDENTITY before launch: fw v0.10.53, serial 6DA08073
DURATION_S = 86400           # 24 hours
RAW_SUB_MS = 50              # raw displacement (Topic 0x03) subscription interval -> row cadence
PHASOR_SUB_MS = 50           # phasors (Topic 0x02) subscription interval
SLOW_POLL_S = 30.0           # temp/status/diag/signal-diag GET poll interval
FIRST_STATUS_S = 300.0       # first console check-in at 5 minutes
STATUS_INTERVAL_S = 1800.0   # then every 30 minutes
OUT_CSV = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-27_24hr_granite_plate_test\data\granite_24hr.csv"

# Resume support: set both to continue appending after a deliberate pause
# (e.g. to free the serial port for a side investigation) instead of
# truncating 2+ hours of already-collected data. Read the last row's
# index/t_s from the CSV and set these before relaunching; leave at
# (0, 0.0) for a fresh run. idx keeps counting up across the gap; t_s
# jumps by the pause duration (the gap itself isn't interpolated/faked).
RESUME_IDX_OFFSET = 615070
RESUME_T_OFFSET_S = 35438.5  # last logged t_s + the ~20s end-charging-attempt pause
APPEND = RESUME_IDX_OFFSET > 0


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
    # primary record (Topic 0x03, drives row cadence)
    "delta1_mm_raw", "residual1", "delta2_mm_raw", "residual2",
    "quality1_ok", "quality2_ok", "delta_diff_mm_raw", "quality_diff_ok",
    # phasors (Topic 0x02), forward-filled, + how stale they are
    "iB", "qB", "iA", "qA", "iS1", "qS1", "iS2", "qS2", "phasor_age_s",
    # slow channel: always-present fields per Testing/README.md convention
    "onboard_temp_cdeg", "battery_mV", "soc_pct", "usb", "charging", "force_charging",
    # slow channel: acquisition/demod error terms
    "clip_count", "amplitude_fault_count", "input_drop", "output_drop",
    "max_update_gap_ms", "max_gap_at_uptime_ms", "gap_over_threshold_count", "degenerate",
    "adc_frame_deficit", "adc_framing_err", "adc_crc_err", "adc_ring_overflow", "adc_backlog",
    # slow channel: granite-plate calibration cross-check (Topic 0x04)
    "rms_B_mV", "rms_A_mV", "rms_S1_mV", "rms_S2_mV", "phase_S1_deg", "phase_S2_deg",
    "theoretical_tilt1_mm_per_m", "theoretical_tilt2_mm_per_m",
    "event",
]

sess = Session(PORT)
st, data = sess.request(a.OP_SYS_IDENTITY)
print("IDENTITY", st, a.decode_identity(data) if st == 0 else data, flush=True)
if st != 0:
    sys.exit("IDENTITY failed -- wrong port or device not responding")

st, dok = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_DISP_OK))
disp_ok = st == 0 and dok and dok[0]
print("disp_ok at launch:", disp_ok, flush=True)
if not disp_ok:
    sys.exit("displacement demod is not running -- start it before launching this test")

# Baseline diag read -- these are saturating counters that persist across this
# script's runtime (nothing here restarts the demod, so any earlier session's
# clip/fault/drop counts carry over). clip_count/amplitude_fault_count are
# uint16 and can already be pinned at 65535 from earlier bench work today --
# if so, this run cannot observe any FURTHER clip/fault events on that
# specific counter (see docs/wp10_displacement.md's note that these should be
# widened to uint32 eventually). Recorded here so it's not silently mistaken
# for "zero clips over 24h" later.
st, dd = sess.request(a.OP_RAW_DISPLACEMENT_DIAG)
baseline_diag = a.decode_displacement_diag(dd) if st == 0 and dd else None
print("baseline displacement diag:", baseline_diag, flush=True)
if baseline_diag and (baseline_diag["clip_count"] >= 65535 or baseline_diag["amplitude_fault_count"] >= 65535):
    print("WARNING: clip_count and/or amplitude_fault_count already saturated at launch "
          "(carried over from earlier today's bench work) -- this run cannot see further "
          "increments on a saturated counter; treat those two columns as 'already pinned', "
          "not 'zero events during this test'.", flush=True)

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

f = open(OUT_CSV, "a" if APPEND else "w", newline="")
w = csv.writer(f)
if not APPEND:
    w.writerow(FIELDS)
else:
    print(f"RESUMING: appending from idx={RESUME_IDX_OFFSET}, t_s offset={RESUME_T_OFFSET_S}s "
          f"({RESUME_T_OFFSET_S/60:.1f}min)", flush=True)

t0 = time.time()
idx = RESUME_IDX_OFFSET
last_slow = -SLOW_POLL_S
next_status = FIRST_STATUS_S if not APPEND else (RESUME_T_OFFSET_S + STATUS_INTERVAL_S)


def now():
    return time.time() - t0 + RESUME_T_OFFSET_S
prev_raw_seq = None
prev_phasor_seq = None
raw_seq_gaps = 0
phasor_seq_gaps = 0

# latest forward-filled state
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
pending_event = "test_start" if not APPEND else \
    f"RESUMED_AFTER_PAUSE(gap~{RESUME_T_OFFSET_S - 35418.545:.0f}s, attempted END_CHARGING -- had no effect, not a forced-charge session)"

# rolling window for status summaries: (t, d1, d2, ddiff)
window = []

print(f"STATUS t={now()/60:.1f}min n={idx} -- 24h granite-plate monitor "
      f"{'resuming' if APPEND else 'starting'}, PORT={PORT}", flush=True)

try:
    while now() < DURATION_S:
        sess._pump()

        for issue_seq, payload in sess.drain_stream(push_phasor_op):
            dec = a.decode_topic_phasors(payload)
            if dec is None:
                continue
            latest_phasor = dec
            last_phasor_t = now()
            if prev_phasor_seq is not None and ((issue_seq - prev_phasor_seq) & 0xFF) != 1:
                phasor_seq_gaps += 1
            prev_phasor_seq = issue_seq

        for issue_seq, payload in sess.drain_stream(push_raw_op):
            dec = a.decode_topic_raw_displacement(payload)
            if dec is None:
                continue
            t = now()
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

        t = now()

        if t - last_slow >= SLOW_POLL_S:
            st, td = sess.request(a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
            temp = struct.unpack("<h", td[:2])[0] if st == 0 and td and len(td) >= 2 else None

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
            # bound the rolling window's memory growth
            window[:] = [ww for ww in window if ww[0] >= t - STATUS_INTERVAL_S - 60]

        if t >= next_status:
            # stats over the last 30 min (or since start, whichever is shorter)
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
            print(f"STATUS t={t/60:7.1f}min n={idx} raw_seq_gaps={raw_seq_gaps} "
                  f"phasor_seq_gaps={phasor_seq_gaps} "
                  f"temp={(slow['onboard_temp_cdeg']/100 if slow['onboard_temp_cdeg'] is not None else 'NA')}C "
                  f"batt={slow['battery_mV']}mV soc={slow['soc_pct']}% "
                  f"usb={slow['usb']} charging={slow['charging']} "
                  f"S1 mean={m1:.4f} std={s1:.5f} S2 mean={m2:.4f} std={s2:.5f} "
                  f"diff mean={md:.4f} std={sd:.5f} "
                  f"clip={slow['clip_count']} ampfault={slow['amplitude_fault_count']} "
                  f"in_drop={slow['input_drop']} out_drop={slow['output_drop']} "
                  f"max_gap={slow['max_update_gap_ms']}ms gaps>thr={slow['gap_over_threshold_count']}",
                  flush=True)
            next_status += STATUS_INTERVAL_S

        time.sleep(0.01)

except Exception as e:
    pending_event = f"SCRIPT_ERROR:{e}"
    print(f"ERROR at t={now():.1f}s: {e}", flush=True)
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
