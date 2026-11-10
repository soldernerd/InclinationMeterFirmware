"""Interleaved collector over USB HID: phasor stream (gapless, 40.69 batches/s) for --stream-min, then --bulk-n raw-ADC
bulk captures (6144 x 4ch @ 20.83 kHz each), repeated for --hours. Stream and bulk are ADC-exclusive, so they alternate.
Output in ./data: phasors.csv (same columns as the 24h logger), bulk_NNNN_<t>.npy (int32 [6144,4]), events.log."""
import argparse, csv, os, sys, time, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "PythonTestCode"))
import apiv3 as a, hid_test as h, ble_test as b

SUB = a.OP_TOPICS_PHASOR_STREAM_SUBSCRIBE
BATCH = 64
ap = argparse.ArgumentParser()
ap.add_argument("--hours", type=float, default=3.0)
ap.add_argument("--stream-min", type=float, default=20.0)
ap.add_argument("--bulk-n", type=int, default=5)
ap.add_argument("--link", default="ble")
ap.add_argument("--out", default=os.path.join(HERE, "data"))
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)
log = open(os.path.join(args.out, "events.log"), "a", buffering=1)
def say(m):
    s = time.strftime("%H:%M:%S ") + m
    print(s, flush=True); log.write(s + "\n")

if args.link == "ble":
    link = b.BleLink(); link.connect()
else:
    link = h.UsbLink()
T0 = time.time()
f = open(os.path.join(args.out, "phasors.csv"), "a", newline="")
w = csv.writer(f)
if f.tell() == 0:
    w.writerow("index,t_s,seq,cycles,gap_cycles,frame_gap,first_batch,iB,qB,iA,qA,iS1,qS1,iS2,qS2,onboard_temp_cdeg,soc_pct,charging,usb,phase".split(","))
state = dict(temp=None, soc=None, chg=None, usb=None)
rows = 0

def poll(): 
    link.send(a.build(a.OP_SYSTEM_STATE_GET)); link.send(a.build(a.OP_MEASUREMENTS_ONBOARD_TEMP_GET))

def stream(minutes, tag):
    global rows
    prev_seq = prev_fr = None; cycles = 0; gaps = fg = 0; n0 = rows
    link.send(a.build(SUB, a.build_interval(50)))
    t_end = time.time() + minutes * 60; nxt = 0; last = time.time()
    while time.time() < t_end:
        if time.time() >= nxt:
            poll(); nxt = time.time() + 30
        if time.time() - last > 10:
            say("stall -> resubscribe"); prev_seq = prev_fr = None
            link.send(a.build(SUB, a.build_interval(50))); last = time.time()
        o, s, d = link.recv(timeout=0.5)
        if o is None: continue
        if o == a.OP_SYSTEM_STATE_GET and s == 0:
            st = a.decode_system_state_response(d)
            state.update(soc=st["battery_soc_pct"], chg=st["charging"], usb=st["usb_connected"]); continue
        if o == a.OP_MEASUREMENTS_ONBOARD_TEMP_GET and s == 0:
            state["temp"] = a.decode_measurements_onboard_temp_response(d)["value"]; continue
        if o != SUB or s != 0 or len(d) < 36: continue
        e = a.decode_phasor_entry(d[2:36])
        if e is None: continue
        fr = d[0]
        fgap = 0 if prev_fr is None else ((fr - prev_fr - 1) & 0xFF); prev_fr = fr
        if prev_seq is None: gap, first, cycles = 0, 1, e["seq"]
        else:
            step = (e["seq"] - prev_seq) & 0xFFFF; gap, first = step - BATCH, 0; cycles += step
        prev_seq = e["seq"]; gaps += bool(gap); fg += bool(fgap)
        w.writerow([rows, f"{time.time()-T0:.4f}", e["seq"], cycles, gap, fgap, first]
                   + [repr(e[k]) for k in ("iB","qB","iA","qA","iS1","qS1","iS2","qS2")]
                   + [("" if state[k] is None else state[k]) for k in ("temp","soc","chg","usb")] + [tag])
        rows += 1; last = time.time()
        if rows % 400 == 0: f.flush()
    link.send(a.build(a.OP_TOPICS_PHASOR_STREAM_UNSUBSCRIBE)); time.sleep(0.5)
    while link.recv(timeout=0.2)[0] is not None: pass
    f.flush()
    say(f"stream {tag}: {rows-n0} rows ({(rows-n0)/(minutes*60):.2f}/s), batch gaps {gaps}, frame gaps {fg}, soc {state['soc']} chg {state['chg']} temp {state['temp']}")

def bulk_once(idx):
    link.send(a.build(a.OP_BULK_RAW_ADC_START_BULK))
    out = []; acked = False; deadline = time.time() + 30
    while len(out) < a.ADC_BULK_SAMPLE_COUNT and time.time() < deadline:
        o, s, d = link.recv(timeout=1.0)
        if o != a.OP_BULK_RAW_ADC_START_BULK: continue
        if not acked:
            if s != 0: return None, f"NACK {a.STATUS.get(s, s)}"
            acked = True; continue
        if s != 0 or not d: continue
        out.extend(a.decode_bulk_adc_chunk(d)[1])
    if len(out) < a.ADC_BULK_SAMPLE_COUNT:
        link.send(a.build(a.OP_BULK_RAW_ADC_CANCEL_BULK)); return None, f"only {len(out)} samples"
    return np.array(out[:a.ADC_BULK_SAMPLE_COUNT], dtype=np.int32), "ok"

def bulk_set(tag, n):
    ok = 0
    for i in range(n):
        arr, msg = bulk_once(i)
        if arr is None:
            say(f"bulk {tag}#{i}: {msg}")
            if "BUSY" in msg:
                link.send(a.build(a.OP_COMMANDS_TILT_DEMOD_EXECUTE, bytes([0]))); time.sleep(0.5)
                arr, msg = bulk_once(i)
                link.send(a.build(a.OP_COMMANDS_TILT_DEMOD_EXECUTE, bytes([1]))); time.sleep(0.5)
        if arr is not None:
            np.save(os.path.join(args.out, f"bulk_{tag}_{i:02d}_{int(time.time()-T0)}.npy"), arr); ok += 1
        time.sleep(2)
    say(f"bulk {tag}: {ok}/{n} captures")

cyc = 0
try:
    while time.time() - T0 < args.hours * 3600:
        tag = f"c{cyc:02d}"
        bulk_set(tag + "a", args.bulk_n)
        stream(args.stream_min, tag)
        cyc += 1
    bulk_set("final", args.bulk_n)
finally:
    f.close(); link.close(); say("DONE")
