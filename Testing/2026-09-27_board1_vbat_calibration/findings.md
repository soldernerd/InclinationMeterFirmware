# Findings

**Same clean, highly linear result as board 2.** Fitted line
(`dmm_mv = raw_mv * 1.334410 + 20.394`) reproduced all 5 sweep points to
within ±1mV. Current draw stable 85-87mA throughout (no current-limiting
against the 100mA cap).

**Old calibration (bare default, same as board 2 had):** `num=133 den=100
offset=0` (ratio 1.330, no offset) — under-read by a similar amount to
board 2, roughly 30-40mV at the top of the range.

**New calibration applied:** `num=9999 den=7493 offset=20` (ratio
1.334445 — closest achievable to the fitted 1.334410 within the wire
protocol's `u16` 1..10000 bounds). Both boards land on very similar ratios
(board 2: 1.336183, board 1: 1.334445 — within 0.13% of each other) and
similar small offsets (24mV vs 20mV), which is a good sign: it means the
R6/R9 divider's actual resistor-tolerance spread between these two
otherwise-identical boards is small, and the bulk of what needed correcting
was a small, consistent ADC/reference offset rather than a wildly
different divider ratio per board.

**Verification (independent 3-point check, after writing the new values):**

| setpoint | DMM (truth) | board reading | error |
|---|---|---|---|
| 3.6V | 3583.98mV | 3583.33mV | -0.64mV |
| 3.9V | 3882.88mV | 3882.00mV | -0.88mV |
| 4.2V | 4183.76mV | 4185.00mV | +1.24mV |

Sub-1.3mV agreement across the board's normal operating range — both
boards now calibrated to comparable precision.

**Process note:** this run applied the lesson from board 2's sweep (the
`num`/`den` `u16` 1..10000 bounds — see that folder's findings) by solving
for the largest valid `den` up front instead of fixing `den=10000` first;
all three SETs succeeded cleanly on the first attempt, no inconsistent
intermediate state this time.
