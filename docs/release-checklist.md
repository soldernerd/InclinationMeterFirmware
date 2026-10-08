# Release checklist for 1.0.0

What must be true before the version number becomes 1.0.0. Items marked **bench** need a board; the rest can be checked at
a desk. Tick them off in the release commit message or a `docs/release-1.0.0-results.md`. Everything since fw 0.10.64
(displacement pipeline, display filter, precision measurement) and everything since 0.11.0 (API v3, service mode, watchdog,
fault capture) is host-tested but **not yet run on hardware**.

## Desk
- [ ] `make -C tests` is green (host suites, generated-file check, Python client tests).
- [ ] Debug and Release firmware build with zero warnings; CI is green on the release commit (CI workflow is unproven: see its header).
- [ ] `CHANGELOG.md` and the version in `Config/app_version.h` match; the release commit is tagged `v1.0.0`.
- [ ] Release artifacts attached: `.elf`, `.hex`, `.map` of the **Release** build, with SHA-256 sums; the build id in IDENTITY
      matches the tag (no `-dirty`).
- [ ] `docs/api-reference.md` is generated from the tagged spec; LevelApp and the mobile app use API v3.
- [ ] `THIRD_PARTY_NOTICES.md` checked, including the terms of the four u8g2 fonts in use.
- [ ] No open questions in `docs/decisions.md` that affect behaviour.

## Bench: the instrument as a product
- [ ] **Release** build timing: `input_drop` / max scheduler gap (Diagnostics DISPLACEMENT) no worse than the Debug build over
      a 1 h run with the LIVE screen, USB, BLE and UART traffic (the project has so far been flashed as Debug, -O0).
- [ ] 24 h soak, battery powered and on USB: no unexpected reset (HEALTH `reset_cause`, uptime), no `settings_save_failed`,
      no TX drops, display refreshed throughout.
- [ ] Standby: power off from the menu, auto power-off, critical battery; wake by encoder and by USB; the watchdog never
      resets a sleeping instrument (`docs/watchdog-and-faults.md` step 5). Sleep current measured.
- [ ] Charging: start threshold, forced charge, end charging, inhibit, charge complete, USB removal / VBUS blink (10 s grace).
- [ ] The watchdog and fault capture plan of `docs/watchdog-and-faults.md` steps 1-4 and 6-7 (`FAULT_TEST` kinds 1-3).
- [ ] USB, BLE and UART round trips with `hid_test.py`, `ble_test.py`, `uart_test.py` (all checks OK, incl. the 56-byte
      signal-diagnostics topic over USB); `bulk_adc_csv.py` capture; phasor stream without gaps for an hour.
- [ ] DFU: `Reboot to DFU` from the menu and from the API (in service mode), recovery with `dfu_flash.ps1`.
- [ ] Service mode: gating, SERVICE_END, the 10 minute idle end, off after sleep.
- [ ] RTC: set, drift measured over >= 12 h with the 1/256 s counter, `RTC_TRIM` applied and the drift re-measured.
- [ ] EEPROM: a power cut during a settings save (brown-out by a supply, mid `SET`) leaves every page either old or new, never
      garbage (the CRC per page protects against the latter; confirm it).

## Bench: the measurement
- [ ] Live accuracy against a reference tilt on a granite plate: sensitivity `k`, zero (flip calibration), phase (shim
      step) calibrated with the documented procedure for both sensors, two instruments if possible.
- [ ] Display filter and "doubtful" flag behave on a quiet table, with footsteps, with a machine running.
- [ ] Precision measurement: result within the expected scatter, error after 5 s on a disturbed table.
- [ ] Temperature behaviour: warm-up drift documented (the earlier finding: shared electronics drift about 14-15 um/degC).
- [ ] Battery gauge: voltage calibration done on each unit (divider swap); SoC plausible.

## Documentation
- [ ] `docs/operator-manual.md` checked against the real instrument; every *(unverified)* procedure in it (phase, k, clock trim)
      run once and corrected.
- [ ] `README.md` quick start works from a fresh clone (`git clone --recurse-submodules`).
