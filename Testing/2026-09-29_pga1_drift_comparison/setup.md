# 1-hour PGA=1 drift comparison

**Date:** 2026-09-29. **Firmware:** 0.10.58 (temporary — reverted to 0.10.59
immediately after). **Board:** serial 6DA08073, COM5. USB disconnected for
the full run (unlike the 24h granite-plate test, which had USB connected
throughout).

## Purpose

The 24h granite-plate test found a real, temperature-correlated, common-mode
drift component (S1 r=+0.77, S2 r=+0.57 with onboard temp; ~0 on the
differential) and identified the ADC's own PGA=16 stage as the leading
suspect, since the A/B exciter channels (which don't use PGA=16) stayed
essentially perfectly stable all day. This test re-runs the identical
method at PGA=1 on all four channels, to check whether the same
temperature-correlated drift signature survives with that stage effectively
out of the picture.

## Method

`pga1_1hr_test.py` — same flat-CSV layout and field set as
`Testing/2026-09-27_24hr_granite_plate_test/granite_plate_24hr_test.py`
(raw displacement + phasors + full error-term set + signal diag), scaled
down to 1 hour with a 10-minute status cadence instead of 30.

**Hardware/firmware change for this run only:**
`Drivers_App/drv_ads131m04.c`'s `GAIN1_REG_VALUE` temporarily changed from
`0x4004` (PGA=16 on S1/S2, PGA=1 on A/B) to `0x0000` (PGA=1 on all four
channels) — a firmware rebuild+flash, not a runtime setting. To keep the
`x=(S/k-B)/(A-B)` ratio math consistent at the new ADC scale,
`disp_s1/s2_gain_milli` (the software pre-amp compensation constant, an
independent gain stage from the ADC's own PGA) was set to 10000 (10.000x,
the pre-PGA-16-bump value) via the API after flashing — not a firmware
constant, so no separate rebuild needed for that half of the change.

**Reverted immediately after this run**: `GAIN1_REG_VALUE` back to `0x4004`
(fw 0.10.59), `disp_s1/s2_gain_milli` back to 160000/160000 via the API.
This is not the normal operating configuration and was never intended to be
left running.

## Status

Ran the full hour cleanly: 63,140 rows, zero sequence gaps on both streams.
See `findings.md`.
