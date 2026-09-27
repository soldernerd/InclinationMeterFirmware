# Findings

**The noise followed the physical sensor head, not the channel/connector.**

| | before swap (ch3="S1") | before swap (ch0="S2") | after swap (ch3) | after swap (ch0) |
|---|---|---|---|---|
| jumps >0.02mm | 284 | 14 | 0 | 40 |

The physical unit that was noisy (284 jumps) stayed noisy after moving to the
other channel (40 jumps — same unit, different channel); the quiet unit stayed
quiet. This **rules out a channel-specific PCB/connector/ADC-trace explanation
entirely** — it's intrinsic to one specific physical sensor head (a marginal
component, mechanical looseness/wear in that unit's pendulum, or ordinary
manufacturing variance between the two nominally-identical 45-year-old units),
not the wiring or which ADC channel it's read on.

**Caveat:** this run also showed a much bigger overall slow drift (~0.21-0.22mm
range on both channels vs ~0.05-0.09mm in the pre-swap run) and correspondingly
much higher S1/S2 correlation (0.9995 vs 0.58) — almost certainly because a
larger shared thermal/settling excursion (likely from the physical handling
during the swap) dominated this window, swamping the smaller per-channel noise
differences in the correlation metric. The exact jump COUNT for the noisy unit
varied a lot between runs (284 vs 40) — the qualitative "this specific unit is
noisier" conclusion is robust and swap-confirmed, but run-to-run magnitude
comparisons need the same settling-window caveat as the creep analysis in
`../2026-09-27_2hr_charging_drift_test/`.

**Recommendation:** identify which of the two physical sensor units is the
noisy one (mark it) and inspect that unit specifically — not the cable, not
the connector, the sensor head itself. Not yet done.
