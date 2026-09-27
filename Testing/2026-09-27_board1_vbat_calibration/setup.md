# Board 1 (grey) battery-ADC calibration

**Date:** 2026-09-27. **Firmware:** 0.10.53. **Board:** #1 ("grey" 3D-printed
enclosure — physically identical to board #2 "green" otherwise). Same
session as `../2026-09-27_board2_vbat_calibration/`, same method, run right
after — the user moved the already-connected PSU/DMM/ST-Link rig from green
to grey rather than rebuilding it.

## Purpose

Same as board 2's: `vbat_scale_num/den`/`vbat_offset_mv` (Settings
`0x0F`/`0x10`/`0x1C`) had never been bench-calibrated for this specific
board against a real reference — it was carrying the bare post-divider-swap
`DEFAULT_*` values (`num=133 den=100 offset=0`), confirmed identical to what
board 2 had before its own calibration.

## Method

Identical to board 2's — see `../2026-09-27_board2_vbat_calibration/setup.md`
for the full instrument/wiring description. Difference: this time the board
under test is identified by its own firmware serial (`6DA08073`) rather than
by COM port, since the physical rig (PSU + DMM + one ST-Link) was moved from
green to grey rather than duplicated — grey ended up on the same COM port
(COM8) green had used, which was confirmed by querying `IDENTITY` before
touching anything.

`calibrate_vbat_board1.py` — same 5-point sweep (`3.5, 3.7, 3.9, 4.1, 4.2`V),
same 100mA current cap, same 3.5V floor (board 2's sweep found 3.0V causes a
real, non-self-recovering brownout). One improvement made after board 2's
run: solves for the largest `den` that keeps `num <= 10000` up front
(instead of fixing `den=10000` and hoping `num` fits), avoiding the
overflow/`INVALID_PARAMETER` mid-write that briefly left board 2 with a
badly inconsistent calibration.
