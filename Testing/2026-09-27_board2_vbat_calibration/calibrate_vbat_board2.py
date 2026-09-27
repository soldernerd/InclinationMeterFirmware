#!/usr/bin/env python3
"""Battery-ADC calibration sweep for Board 2 (green), using the E36104A PSU
(already connected in place of the LiPo) as the voltage source and the
34465A DMM (in parallel on the same node) as ground truth.

Bounds agreed with the user: PSU setpoint <= 4.3V, current limit = 100mA
(voltage may sag slightly under load -- that's expected and fine, the DMM
is the ground truth, not the PSU's own setpoint).
"""
import struct
import sys
import time

sys.path.insert(0, r"J:\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
from uart_test import UartLink, request
from bench_instruments import Psu, Dmm

PORT = "COM8"   # Board 2 (green)
# 3.0V caused a real brownout (board's own 3.3V LDO can't hold regulation
# down there -- config.h's battery_critical_mv default is 3.40V for exactly
# this reason). Floor kept comfortably above that.
VOLTAGES = [3.5, 3.7, 3.9, 4.1, 4.2]
SETTLE_S = 2.0
N_SAMPLES = 6


def get_i32(link, cat, res):
    st, d = request(link, a.opcode(a.GET, cat, res))
    return struct.unpack("<i", d[:4])[0] if (st == 0 and d and len(d) == 4) else None


def get_u16(link, cat, res):
    st, d = request(link, a.opcode(a.GET, cat, res))
    return struct.unpack("<H", d[:2])[0] if (st == 0 and d and len(d) == 2) else None


def get_battery_mv(link):
    st, d = request(link, a.opcode(a.GET, a.CAT_MEAS, a.MEAS_BATTERY_MV))
    return struct.unpack("<H", d[:2])[0] if (st == 0 and d and len(d) == 2) else None


link = UartLink(PORT)
psu = Psu()
dmm = Dmm()

num0 = get_u16(link, a.CAT_SETTINGS, a.SET_VBAT_SCALE_NUM)
den0 = get_u16(link, a.CAT_SETTINGS, a.SET_VBAT_SCALE_DEN)
off0 = get_i32(link, a.CAT_SETTINGS, a.SET_VBAT_OFFSET_MV)
print(f"Board 2 current calibration: num={num0} den={den0} offset={off0}mV")

psu.set_current_limit(0.100)

results = []
for v in VOLTAGES:
    psu.set_voltage(v)
    time.sleep(SETTLE_S)

    dmm_samples = [dmm.measure_vdc() for _ in range(N_SAMPLES)]
    dmm_mv = sum(dmm_samples) / len(dmm_samples) * 1000.0

    bat_samples = []
    for _ in range(N_SAMPLES):
        bv = get_battery_mv(link)
        if bv is not None:
            bat_samples.append(bv)
        time.sleep(0.15)
    board_mv = sum(bat_samples) / len(bat_samples) if bat_samples else None

    psu_i = psu.measure_current()
    psu_v = psu.measure_voltage()

    if board_mv is None:
        print(f"  setpoint={v}V  psu_v={psu_v:.4f}V psu_i={psu_i*1000:.2f}mA  "
              f"dmm={dmm_mv:.2f}mV  BOARD NOT RESPONDING -- skipping this point")
        continue

    # Back out the raw (pre-correction) ADC-domain mV using the CURRENT
    # calibration, so the regression below fits against the true ADC input,
    # not against an already-corrected number.
    raw_mv = (board_mv - off0) * den0 / num0

    results.append(dict(setpoint=v, dmm_mv=dmm_mv, board_mv=board_mv, raw_mv=raw_mv,
                         psu_v=psu_v, psu_i=psu_i))
    print(f"  setpoint={v}V  psu_v={psu_v:.4f}V psu_i={psu_i*1000:.2f}mA  "
          f"dmm={dmm_mv:.2f}mV  board_reported={board_mv:.2f}mV  raw_adc={raw_mv:.2f}mV")

# ---- Linear regression: dmm_mv = raw_mv * slope + intercept ----
n = len(results)
sx = sum(r["raw_mv"] for r in results)
sy = sum(r["dmm_mv"] for r in results)
sxx = sum(r["raw_mv"] ** 2 for r in results)
sxy = sum(r["raw_mv"] * r["dmm_mv"] for r in results)
slope = (n * sxy - sx * sy) / (n * sxx - sx ** 2)
intercept = (sy - slope * sx) / n

print(f"\nFit: dmm_mv = raw_mv * {slope:.6f} + {intercept:.3f}")

DEN_NEW = 10000
num_new = round(slope * DEN_NEW)
offset_new = round(intercept)
print(f"New calibration: num={num_new} den={DEN_NEW} offset={offset_new}mV "
      f"(ratio {num_new/DEN_NEW:.6f})")

# residuals with the new fit, for a sanity check before writing
print("\nResiduals with new fit (should be small, a few mV):")
for r in results:
    predicted = r["raw_mv"] * num_new / DEN_NEW + offset_new
    print(f"  setpoint={r['setpoint']}V  dmm={r['dmm_mv']:.2f}mV  "
          f"predicted={predicted:.2f}mV  residual={predicted - r['dmm_mv']:+.2f}mV")

psu.close()
dmm.close()
link.close()

import json
with open(r"C:\Users\lfaes\AppData\Local\Temp\claude\J--OneDrive-EmbeddedSystems-InclinationMeterFirmware\5d47c327-9528-4f27-a1e4-7d3065fb5850\scratchpad\vbat_cal_results.json", "w") as f:
    json.dump(dict(old=dict(num=num0, den=den0, offset=off0),
                   new=dict(num=num_new, den=DEN_NEW, offset=offset_new),
                   points=results), f, indent=2)
print("\nSaved raw results to vbat_cal_results.json")
