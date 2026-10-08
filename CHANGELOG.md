# Changelog

Firmware versions are `FW_VERSION_*` in `Config/app_version.h`. Until 1.0.0 the minor number moves with breaking changes
of the wire protocol or the stored settings; the patch number with every build handed over for flashing. Entries are
newest first. **Nothing from 0.10.64 on has been bench-tested yet** unless a line says so; the acceptance list for 1.0.0 is
`docs/release-checklist.md`.

## 0.11.1 - 2026-10-08
- Service mode: calibration writes and the maintenance commands (zero calibration, factory defaults, DFU, power / pin test,
  rails, fault test) need it. It can only be entered on the instrument; it ends by menu, `SERVICE_END`, 10 minutes without API
  requests, or sleep.
- Supervised window watchdog (WWDG refreshed from SysTick while the main loop makes progress, 3 s stall limit).
- HardFault capture: PC / LR / xPSR / SP kept across the reset, logged at the next boot, in System HEALTH.
- New `FAULT_TEST` command to exercise the above on the bench.
- The firmware reports a build id (git commit, `-dirty`) in IDENTITY and on the STATUS screen.
- Host tests added for the battery policy, EEPROM storage (against a fake chip) and the BME280 compensation.

## 0.11.0 - 2026-10-08
- **API v3**: the whole API regrouped and generated from `tools/api_spec.py` (`docs/api-v3-spec.md`, `docs/api-reference.md`).
  Breaking: new category layout, state-changing IDs >= 0x40, new Python client `apiv3.py`.
- New: System HEALTH, RTC sub-second and trim (`RTC_TRIM`), Topics LIVE, Procedures, event subscriptions, USB multi-report
  fragmentation, power off / reboot / factory defaults / rail commands.
- The "Display rate" setting (it paced nothing) was removed; the scheduler EEPROM page is reseeded once.

## 0.10.72 - 2026-10-07
- Review clean-up phases 0-5: bug fixes (idempotent start, exclusivity, topic buffer overflow, ADC frame ring off-by-one,
  quality floor readiness / re-seed, zero-cal range check), removal of the output ring, the 8-batch moving average, the
  per-batch quality flag and the bulk phasor log; the per-batch maths moved to `Math/math_displacement` and
  `Math/math_quality` with a replay test on recorded data.

## 0.10.71 and earlier
See `git log` and `docs/wp10_displacement.md` (displacement / tilt history), `docs/signal_processing.pdf` (theory and the
analysis of the 19 h recording).
