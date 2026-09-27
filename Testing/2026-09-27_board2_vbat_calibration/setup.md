# Board 2 (green) battery-ADC calibration

**Date:** 2026-09-27. **Firmware:** 0.10.53. **Board:** #2 ("green" 3D-printed
enclosure — physically identical to board #1 "grey" otherwise).

## Purpose

Board 2's battery voltage reading (`Measurements 0x01`, `BATTERY_MV`) had never
been bench-calibrated against a real reference — the existing
`vbat_scale_num/den`/`vbat_offset_mv` (Settings `0x0F`/`0x10`/`0x1C`) were
whatever board 1's earlier calibration/EEPROM defaults happened to carry
(`num=133 den=100 offset=0`, i.e. ratio 1.330, no offset).

## Method

The Keysight E36104A bench PSU was already connected in place of Board 2's
LiPo cell (battery disconnected), and the Keysight 34465A DMM was already
connected in parallel across that same node as ground truth —
`PythonTestCode/bench_instruments.py`'s `Psu`/`Dmm` wrappers (PyVISA/SCPI).

Agreed bounds for this sweep: PSU setpoint ≤ 4.3V (the project's standing
bench safety limit, `docs/bench_instruments.md`), current limit 100mA.

`calibrate_vbat_board2.py`: for each setpoint in `[3.5, 3.7, 3.9, 4.1, 4.2]`V
— set the PSU, settle 2s, average 6 DMM readings and 6 `BATTERY_MV` GETs over
UART (COM8). The board's own CURRENT (pre-calibration) `num`/`den`/`offset`
are used to back out the raw ADC-domain millivolt value from the
already-corrected `BATTERY_MV` reading (`raw_mv = (board_mv - offset) *
den / num`), so the regression fits against the true ADC input, not against
an already-corrected number. Linear least-squares fit: `dmm_mv = raw_mv *
slope + intercept`.

**3.0V was tried first and dropped** — it caused a real brownout (the
board's own 3.3V LDO can't hold regulation that close to the LiPo's nominal
range; `config.h`'s `battery_critical_mv` default is 3.40V for exactly this
reason) that did **not** self-recover — the board needed an explicit SWD
reset (`STM32_Programmer_CLI -c port=SWD mode=UR -rst`) to come back, UART
alone didn't bring it back. Swept range moved to 3.5-4.2V, comfortably above
that.

`vbat_cal_sweep.csv` — the 5-point sweep used for the fit.
`vbat_cal_verification.csv` — a second, independent 3-point check (3.6/3.9/4.2V)
run AFTER writing the new calibration, to confirm it actually holds.
