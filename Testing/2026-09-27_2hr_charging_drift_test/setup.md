# 2-hour charging/drift test

**Date:** 2026-09-27. **Firmware:** 0.10.49. **Board:** #2.

## Purpose

User asked for a long (1-2h) unattended drift measurement once self-heating
stabilizes, with regular intermediate reports, plus a separate ask: "how does
noise change when charging? USB is connected so try with and without." Also
motivated by a challenge to the earlier shared-drift explanation: the
ratiometric math (`x=(S/k-B)/(A-B)`) should cancel shared amplitude/reference
drift — what temperature-dependent term could survive that? (See
`docs/wp10_displacement.md` for the full theoretical discussion; conclusion:
`k=atten*gain` is a fixed SOFTWARE constant, not measured live, so an
asymmetric drift specific to the S-channel's own gain path would NOT cancel.)

Before running: a smoke-test force-charge trigger had left the board charging
with no way to cancel it via the API except reaching full charge or physically
unplugging USB. Implemented `EXECUTE 0x1/0x08` "End charging"
(`svc_battery_cancel_force_charge()`) to unblock the test.

## Method

`long_drift_charge_test.py` — `DURATION_S=7200`, `CHARGE_ON_AT_S=3600`,
`STATUS_INTERVAL_S=300`. Topic 0x5/0x03 (raw displacement) streamed
continuously via `SUBSCRIBE` for the full 2 hours: 60 minutes natural
(not-charging) baseline, then `EXECUTE 0x1/0x0?` force-charge for 60 minutes.
Incrementally-flushing CSV writers so partial data survives if the run is
interrupted; printed `STATUS`/`EVENT` lines for live progress reporting during
the run.

Two output files (same fast/slow split as the streaming-monitor tests):
- `long_drift_stream.csv` — fast stream: `t_s,issue_seq,delta1_mm_raw,residual1,delta2_mm_raw,residual2,quality1_ok,quality2_ok`
- `long_drift_slow.csv` — slow side-channel: `t_s,onboard_temp_cdeg,charging,usb,battery_mv,clip_count,amplitude_fault_count,input_drop,output_drop,max_update_gap_ms,gap_over_threshold_count,event`

`auto_poweroff_s` raised beforehand (see [[board2-standby-during-bench]]) so
the board wouldn't sleep mid-run.

**Caveat:** battery was at 98% SoC (charged overnight before the test), so
forced charging here likely drew less current/heat than a mid-charge (e.g.
50%) session would — the "charging" phase may understate a real recharge's
thermal/noise impact.
