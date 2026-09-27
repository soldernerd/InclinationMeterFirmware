# Findings

**Clean, highly linear fit.** Across 3.5-4.2V, the fitted line
(`dmm_mv = raw_mv * 1.336243 + 24.036`) reproduced all 5 sweep points to
within ±1mV — the battery divider + ADC are behaving ideally; the old
calibration's error was a fixed multiplicative/offset mismatch, not any
nonlinearity.

**Old calibration was off by a real, consistent amount:** `num=133 den=100
offset=0` (ratio 1.330, no offset) vs. the fitted 1.336243 ratio + 24mV
offset — the old settings under-read by roughly 15-40mV across this range
(e.g. at 4.2V: old calibration would report ~4143mV against a true 4184mV,
a ~41mV/1% error).

**New calibration applied:** `num=10000 den=7484 offset=24` (ratio
1.336183 — the closest achievable to the fitted 1.336243 within the wire
protocol's `u16` 1..10000 bounds on both `num` and `den`).

**Verification (independent 3-point check, after writing the new values):**

| setpoint | DMM (truth) | board reading | error |
|---|---|---|---|
| 3.6V | 3583.87mV | 3584.00mV | +0.13mV |
| 3.9V | 3882.74mV | 3883.00mV | +0.26mV |
| 4.2V | 4183.63mV | 4183.00mV | -0.63mV |

Sub-millivolt agreement across the board's normal operating range.

**Real hardware finding, not just a calibration note:** feeding Board 2 at
3.0V (attempting to extend the sweep down to the LiPo's absolute floor)
caused a genuine brownout — the board went unresponsive on UART and stayed
that way even after the PSU was raised back to 3.9V. It needed an explicit
SWD connect-under-reset + reset (`-rst`) to resume; it did **not**
self-recover on its own. `PWR_CURRENT` during the brownout dropped from
~85mA (normal) to ~0.4mA, confirming the MCU/regulator had actually dropped
out, not just misbehaved. Consistent with (and a real confirmation of)
`config.h`'s `battery_critical_mv` default of 3.40V being set with real
margin above the hardware's actual operating floor, not an arbitrary
number.

**Not yet done:** the same calibration for Board 1 (grey) — this session
only touched Board 2. Board 1's `vbat_scale_num/den/offset` should be
checked the same way if/when it's convenient (they may already be
reasonably calibrated — `battery-voltage-calibrated.md` in project memory
has that board's own history).
