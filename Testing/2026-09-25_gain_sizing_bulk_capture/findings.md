# Findings

**PGA=1 baseline** (`gain_check_pga1.csv`): S1/S2 (ch3/ch0) measured only
~52-58mV peak + ~7mV DC offset — ~2.5% of the 2.4V full scale. A/B (ch1/ch2)
measured ~1250mV — ~52% of full scale, already near their own ceiling.

**Decision:** PGA=16 on S1/S2 only (leaves A/B untouched). At 16x, S1/S2 use
~43% of the new, smaller (150mV) full scale — comfortable margin, and a real
16x resolution improvement. PGA=32 was considered and rejected: would already
use ~87% of a 75mV full scale on the same measured signal, too tight given the
measurement was an uncontrolled bench tilt, not a certified reference.

**PGA=16 verification** (`gain16_check_pga16.csv`), same board/setup, taken
right after applying the change: S1/S2 peak-to-peak scaled by almost exactly
**16.00x** (104.66mV→1673.27mV on S1, 114.96mV→1842.28mV on S2). A/B unchanged
(within capture-to-capture noise) — confirms the gain landed exactly on the
intended two channels only.

Implemented as `Drivers_App/drv_ads131m04.c`'s `GAIN1_REG_VALUE = 0x4004`
(fw 0.10.41), with a matching 16x bump to `DEFAULT_DISP_S1/S2_GAIN_MILLI`
(the software gain-compensation constant) since the ADC's own PGA and the
software `gain` calibration constant are independent — see
`Config/config.h`'s comment on that default for the full derivation.
