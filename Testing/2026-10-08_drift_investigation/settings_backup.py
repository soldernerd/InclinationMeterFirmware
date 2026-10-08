"""Back up / restore / show every Settings (cat 0x3) and Calibrations (cat 0x2) resource of the board over the wired UART.
A firmware change that bumps an EEPROM page version reseeds that page to defaults on the first boot (fw 0.10.75 does so for
the battery page: thresholds, ADC scale/offset, auto power-off), so:   save BEFORE flashing, restore AFTER.
Resources the new firmware does not have (UNKNOWN_RESOURCE) keep their new default; the restore verifies each write.

python settings_backup.py save  [file.json]      (default: data/settings_backup_<time>.json)
python settings_backup.py restore file.json
python settings_backup.py show
"""
import json
import os
import sys
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "PythonTestCode"))
import apiv2 as a   # noqa: E402
import serial       # noqa: E402


def find_port():
    import serial.tools.list_ports as lp
    for p in lp.comports():
        if "STLink" in (p.description or "") or "ST-Link" in (p.description or ""):
            return p.device
    sys.exit("no ST-Link virtual COM port found")


class Link:
    def __init__(self):
        self.ser = serial.Serial(find_port(), 115200, timeout=0.3)
        self.ser.reset_input_buffer()
        self.ra = a.Reassembler()

    def xfer(self, op, payload=b"", timeout=1.5):
        self.ser.reset_input_buffer()
        self.ser.write(a.build(op, payload))
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(300)
            for o, st, d in (self.ra.feed(chunk) if chunk else []):
                if o == op:
                    return st, d
        return None, None


def read_all(link):
    out = {}
    for cat, name in ((a.CAT_SETTINGS, "settings"), (a.CAT_CALIB, "calib")):
        for res in range(0x00, 0x24):
            st, d = link.xfer(a.opcode(a.GET, cat, res))
            if st == 0 and d is not None:
                out[f"{name}:{res:02X}"] = d.hex()
    return out


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("save", "restore", "show"):
        sys.exit(__doc__)
    link = Link()
    st, d = link.xfer(a.OP_SYS_IDENTITY)
    if st != 0:
        sys.exit("IDENTITY failed (is another program using the port?)")
    ident = a.decode_identity(d)
    print("board:", ident)
    cur = read_all(link)
    if sys.argv[1] == "show":
        for k, v in cur.items():
            n = len(v) // 2
            sig = int.from_bytes(bytes.fromhex(v), "little", signed=True) if n <= 4 else None
            print(f"  {k}  {n} B  {v}" + (f"  = {sig}" if sig is not None else ""))
        return
    if sys.argv[1] == "save":
        fn = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "data", datetime.now().strftime("settings_backup_%Y%m%d_%H%M%S.json"))
        os.makedirs(os.path.dirname(os.path.abspath(fn)), exist_ok=True)
        json.dump(dict(identity=str(ident), values=cur), open(fn, "w"), indent=1)
        print(f"saved {len(cur)} resources -> {fn}")
        return
    saved = json.load(open(sys.argv[2]))["values"]
    bad = 0
    for key, hexval in saved.items():
        name, res = key.split(":")
        cat = a.CAT_SETTINGS if name == "settings" else a.CAT_CALIB
        res = int(res, 16)
        if key not in cur:
            print(f"  {key}: not present in this firmware, skipped")
            continue
        if cur[key] == hexval:
            continue
        st, _ = link.xfer(a.opcode(a.SET, cat, res), bytes.fromhex(hexval))
        st2, back = link.xfer(a.opcode(a.GET, cat, res))
        ok = (st == 0 and st2 == 0 and back is not None and back.hex() == hexval)
        bad += 0 if ok else 1
        print(f"  {key}: {cur[key]} -> {hexval} [{a.STATUS.get(st, st)}] {'OK' if ok else 'MISMATCH'}")
    print("restore complete" + ("" if not bad else f" WITH {bad} PROBLEMS"))


if __name__ == "__main__":
    main()
