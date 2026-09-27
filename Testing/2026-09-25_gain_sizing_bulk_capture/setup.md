# Gain sizing bulk capture

**Date:** 2026-09-25/26. **Firmware:** 0.10.35 (PGA=1 capture) then 0.10.41 (PGA=16
capture). **Board:** #2, serial `6DA08073`.

## Purpose

Before setting the ADS131M04's PGA gain on S1/S2, needed real bench numbers for
how much of the ADC's full scale the sensor signals actually use — the
question that started this: "there is a PGA in the ADC, what gain setting can
we afford?"

## Method

`PythonTestCode/bulk_adc_csv.py` — a Bulk transfers (`0x8/0x00`) raw-ADC
capture: 6144 samples x 4 channels, ~295ms, at the ADC's full ~20833Hz rate.
Requires the real-time displacement demod stopped first (`EXECUTE 0x1/0x01`
payload `0`) since the two are mutually exclusive (both want the ADS131M04's
one sample-callback slot).

- `gain_check_pga1.csv` — captured at the stock PGA=1 setting (fw 0.10.35, before
  the gain change).
- `gain16_check_pga16.csv` — captured immediately after setting
  `GAIN1_REG_VALUE = 0x4004` (PGA=16 on S1/S2, gain=1 unchanged on A/B),
  same board, same physical setup, no time gap.

Each row: one sample, `ch0,ch1,ch2,ch3` as raw signed 24-bit ADC codes
(ch0=S2, ch1=B, ch2=A, ch3=S1 — see `Services/svc_displacement.h`'s channel
mapping comment).
