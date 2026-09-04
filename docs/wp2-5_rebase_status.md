# WP2–WP5 Rebase onto REV B — Status

**Goal:** rebase the `wp2`, `wp3`, `wp4`, `wp5` feature branches onto the REV B `master`
(pinout finalized in `STM32G0B1RET6_Pinout.csv`), dropping each branch's now-obsolete
CubeMX-regen-only commit, and fixing code that breaks from genuine REV B behavioral changes —
not just renamed pins. One branch at a time, in dependency order, since each branch was
originally stacked on the previous one. Stop after `wp5` builds clean; do not merge into
`master`.

**Read first:** `docs/pinout_migration_wp2-5.md` and `docs/cubemx_configuration_checklist.md`
for the full REV B migration record this rebase is checked against.

**Method used:** interactive rebase (`git rebase -i`) isn't available in this tooling
(requires an interactive editor). Equivalent result achieved per branch via:
```
git checkout <branch>
git reset --hard <new-base>          # moves the branch pointer, doesn't touch commits' history
git cherry-pick <feature-commit-1> [<feature-commit-2> ...]   # regen commit deliberately omitted
```
Original commits remain reachable via `git reflog` on whichever machine did the reset until
GC runs; nothing is deleted immediately.

---

## Status

| Branch | State | Base | Notes |
|---|---|---|---|
| `wp2` | **Rebased a second time (onto post-WP1-REV-B-port `master`), extensively extended beyond the original checklist, build-verified, two full code-review passes complete. Ready to push as of `ba2aa2c`.** | `master@f402d2b` | Feature commit `0e7abd1` cherry-picked, then 7 more commits of fixes/features (see "wp2 — post-rebase work" below). `wp2` HEAD is `ba2aa2c`, 9 commits ahead of `origin/wp2`. |
| `wp3` | **Rebased onto `wp2@83c4617` (cherry-pick `f2646e4`, `880c218` dropped), full scope (including the UI state machine) implemented and hardware-adapted, EEPROM storage redesigned into per-subsystem pages — see "wp3 — resolution" and "Code review" below. Build-verified clean; three code-review passes complete (first: 8-angle/12-verified/10-fixed; second: partial-angle plus the EEPROM per-page redesign; third: remaining angles against that redesign, found and fixed a real data-corruption bug), 2026-08-17.** | `wp2@83c4617` | Feature commit `f2646e4` cherry-picked + 13 follow-up commits (Core/ EXTI wiring, svc_input/system_state/display, UI state machine restoration, three code-review fix rounds, EEPROM per-page redesign, docs throughout). `wp3` HEAD is `8f9d3e8`. |
| `wp4` | **Hand-adapted onto `wp3@2d5fc7e`, reconciled against two real CubeMX regens (the second with NVIC properly configured via the GUI), through two full code-review passes (9+8 findings, 17 fixed total) — see "wp4 — resolution and adaptation notes" below. Build-verified clean, 2026-08-18.** | `wp3@2d5fc7e` | `wp4` HEAD is `c7665dc`. |
| `wp5` | Not started | `wp4` (once pushed) | Commits `1662959`, `73aa050`, decide on `81a9643` (docs-only, likely stale post-REV-B, review before keeping); drop `4bc4f16`. |

**Build verification:** no ARM toolchain existed on the machine `wp3` was developed on
(confirmed by an exhaustive filesystem search; the `build/` directory previously checked
into this working tree was a stale artifact from a different machine/user profile,
`C:/Users/lfaes/...`) — every file was reviewed by hand first. `arm-none-eabi-gcc` 14.2.1,
CMake 4.4.2, and Ninja 1.13.2 (exact same versions `wp2` was built with) were then installed
via `winget` on this machine, the stale `build/` directory removed, and a fresh
`cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake` +
`cmake --build build` run clean: **all 207 objects compile and link with zero warnings under
`-Wall -Wextra -Werror`** (RAM 20.6%, Flash 15.2%). **Still not flashed to real hardware** —
see the outstanding items in "wp2 — post-rebase work" below, which apply equally to `wp3`.

---

## The 6 known-issue checklist (from the original rebase task)

Check each explicitly for every branch — don't assume a clean cherry-pick:

1. **DISP_CS** — SPI2 hardware NSS can't drive the display's active-HIGH CS; must be
   manual GPIO assert/deassert around each SPI transaction.
2. **CHARGE_EN** — re-derive active level from the CSV's Comments column
   (`!CHARGE_EN!`, Low = charge) against whatever branch's charge-control code; don't assume
   old polarity.
3. **HSE 8 MHz** (was 16 MHz on old boards) — grep for hardcoded frequency assumptions
   outside CubeMX-generated init code (manual delay loops, baud-rate math, prescaler calcs).
4. **BATTERY_SENSE / TEMP_SENSE / TEMP_SENSE_EXT** — new pins, new ADC1 channel numbers,
   and a fixed ascending-channel-number scan order (not GUI rank order) — see `wp2`'s
   `hal_adc.c` for the pattern.
5. **DEBUG_UART TX/RX swap** — verify against whichever branch first touches
   `HAL_App/hal_uart.c` (WP1.5 scope — may not be in any of these branches at all; wp2's
   diff didn't touch it).
6. **PB10 dual-use (ADC_CLOCK vs. DAC_SCK)** — resolved by REV B moving DAC_SCK to PC10;
   grep for leftover PB10-as-DAC_SCK references to confirm.

## wp2 — resolution against the checklist

1. N/A directly (wp2 doesn't own the display), but fixed anyway — see "pre-existing bug"
   below.
2. N/A — `CHARGE_EN` isn't referenced anywhere in wp2's code (REV A hardware had no
   charge-enable control at all, only CHG sense). Re-check for wp3/4/5.
3. Checked, clean — no hardcoded HSE-frequency assumptions in wp2's diff.
4. **Fixed** — `hal_adc.c` scan order corrected to CH3→CH13(VREFINT)→CH15→CH16 (ascending,
   not GUI order); `pin_config.h` macros repointed to PA3/PB12/PB11.
5. N/A — debug UART isn't touched by wp2.
6. Checked, clean — no leftover PB10-as-DAC_SCK references.

## wp2 — fixes beyond mechanical renames

- **3V3_EN polarity inversion** (highest-severity item flagged in the original migration
  survey): old `LDO_EN` (PC0) active-HIGH → REV B `!3V3_EN!` (PC6) active-LOW. Fixed in
  `main.c`'s pre-`HAL_Init()` boot code and in `svc_battery.c`'s shutdown latch (which
  needed its boolean flipped, not just the pin renamed — a straight rename would have left
  the rail permanently on instead of shutting down on critical battery).
- **ADC1 scan order** — REV B's fixed/`NOT_FULLY_CONFIGURABLE` sequencer always converts in
  ascending channel-number order regardless of CubeMX's rank list. `hal_adc.c`'s DMA buffer
  indices now match reality.
- **Pin moves**: `BATTERY_SENSE`→PA3/IN3, `TEMP_SENSE`→PB12/IN16, `CHARGE_SENSE`→PC2,
  `VBUS_SENSE`→PA2, `LED_PWR`/`LED_STATUS`→PB13/PB14.
- **VBUS_SENSE (PA2) digital-read gap** — it's a `SYS_WKUP4` pin with no CubeMX-generated
  `GPIO_Init()` call, left in POR-default Analog mode. `hal_gpio_init()` now explicitly
  reconfigures it as a digital input.
- **5V_EN** — new REV B rail, required for `TEMP_SENSE`/`TEMP_SENSE_EXT` to read correctly.
  No power-sequencing module exists yet; asserted unconditionally at boot alongside 3V3 as a
  flagged stopgap, not a real reference-counted design.
- **Pre-existing WP1 bug fixed while in the area** (not part of wp2's diff, but `pin_config.h`
  and the display pins were already being rewritten): `hal_spi.c` was still wired to
  `hspi1`/`SPI1` for the display. REV B moved the display to SPI2 (SPI1 is now the external
  ADC front-end).
- **TMP236 vs. LM35 — RESOLVED (2026-08-17).** REV B's on-board temp sensor (`TEMP_SENSE`)
  changed parts to a TI TMP236; `drv_lm35.c`/`drv_lm35.h` renamed to `drv_tmp236.c`/`.h` and
  reimplemented against TI's published piecewise-linear transfer function (datasheet
  SBOS857E, Table 2). Also added `HAL_App/hal_adc.c`'s `hal_adc_raw_to_mv()`, a
  VREFINT-ratiometric raw-code-to-millivolts conversion — REV B ties VREF+ directly to the
  3V3_STANDBY rail rather than a fixed-voltage VREFBUF, so the old fixed-2.048V-reference
  shortcut this formula used to rely on was wrong regardless of which sensor part is fitted.
  `TEMP_SENSE_EXT` (the new external channel) is still expected to be a classic LM35 —
  documented in `pin_config.h` (`LM35_SCALE`, now 10 mV/°C with no offset, corrected for the
  same VREFINT-ratiometric approach) but **no driver exists for it yet**, that's separate
  future work. Both temp sensors are powered from the 5V rail (`PWR_5V_EN`) — also now
  documented at each pin's definition in `pin_config.h`.

## wp2 — post-rebase work (2026-08-16 to 2026-08-17), well beyond the original checklist

After the mechanical rebase above, `wp2` was rebased a *second* time onto the post-WP1-
REV-B-port `master` (`f402d2b`, which includes the VCOM/SPI2/rail-sequencing fixes from
the "Known critical bug" section below) so it would inherit those fixes instead of
duplicating them under different macro names. `wp2` then grew substantially past what the
original rebase task scoped, driven by direct user requirements:

- **TMP236 on-board temp sensor implemented** (`ace2f11`) — `drv_lm35.c` renamed to
  `drv_tmp236.c`, reimplemented against TI's actual datasheet (SBOS857E) piecewise-linear
  transfer function, not a guess. Added `hal_adc_raw_to_mv()`, a VREFINT-ratiometric
  raw-code-to-mV conversion needed because REV B ties VREF+ to the 3V3_STANDBY rail rather
  than a fixed reference.
- **Full power management implemented** (`be97f0a`) — TP4056 charge control
  (`CHARGE_SENSE`/`STANDBY_SENSE` read, `CHARGE_EN` driven), a critical-battery →
  STM32 Standby-mode shutdown sequence (`HAL_App/hal_power.c`, new), 3 Standby wake-up
  pins (`ENC_1SW`/`VBUS_SENSE`/`ENC_2SW`, all high-level triggered).
- **Battery thresholds switched from SOC% to direct voltage** (`a63ecf0`) — user-specified
  `battery_critical_mv`=3650, `battery_low_mv`=3800. Also corrected the Vbat divider ratio
  (100k/33k, not the previously-assumed 100k/68k) and flagged that the divider is wired
  backwards from optimal (safe, but wastes ~2/3 of the ADC's usable range — noted for the
  next hardware rev, not reworked on existing boards).
- **All calibration constants moved to EEPROM** (`1155765`) — project rule, stated
  explicitly by the user: "no such calibration in flash." `vbat_scale_num/den`, all 8
  TMP236 piecewise-formula constants, and `lm35_scale_mv_per_c` are now `DeviceSettings`
  fields (EEPROM-backed) with `DEFAULT_*` seeds in `config.h`, not bare `#define`s.
  `EEPROM_SETTINGS_VERSION` bumped twice in this stretch of work (0x0001→0x0002→0x0003).
- **TP4056 charge policy refined against the actual datasheet** (`1155765`) — charging
  only starts once Vbat drops to `battery_low_mv` (avoids topping off an already-near-full
  LiPo, which degrades it), `CHARGE_EN` now turns off on charge-complete (not just on USB
  loss), and `CHARGE_SENSE`/`STANDBY_SENSE` are only trusted while `VBUS_SENSE` is present
  (the TP4056 is powered from VBUS — its outputs are undriven without it).
- **Two full code-review passes** (`/code-review`, high effort, 8-angle) — first pass (10
  findings, all fixed in `31381f7`) covered the original mechanical-rebase content; second
  pass (10 more findings, after all the work above) caught real bugs: an ADC race mixing
  raw values across scans, an EEPROM I2C timeout that could corrupt a caller's stack via a
  late DMA completion, a dropped fail-safe (0mV reading silently became "normal" instead
  of "critical"), an ADC error path that never invalidated stale data, `CHARGE_EN`
  inheriting an unsafe boot-time default, and more — all fixed in `cc024a1`.
- **Standby-mode GPIO/rail-retention gap — confirmed as a real REV B hardware mistake,
  not a firmware guess** (`ba2aa2c`). The second review flagged that `HAL_PWR_EnterSTANDBYMode()`
  doesn't preserve GPIO output state through actual Standby entry. User checked the
  schematic and confirmed: `PWR_3V3_EN` (PC6) drives a P-MOSFET gate directly with no
  external pull-up, and `PWR_5V_EN` (PC7) feeds a regulator's active-low shutdown input
  ("must not be allowed to float" per its datasheet) with no external pull-down — a real
  design gap on already-built boards. Mitigated in firmware using STM32's Standby I/O
  retention (`PWR_PUCRx`/`PDCRx` + APC): `hal_power_configure_rail_retention()` holds PC6
  pulled up and PC7 pulled down through Standby using the MCU's own weak internal pulls.
  Flagged in `pin_config.h` for a real fix (board-level pull resistors) on the next rev.

**Still outstanding — nothing here has touched real hardware yet:**
- Standby entry/wake cycle itself (does it actually reach ~0.28 µA with the rail-retention
  pulls active; do all 3 wake sources actually wake the device; does I/O retention behave
  as expected on real silicon).
- TP4056 charge-hysteresis behavior (start-at-low-threshold, latch-until-complete).
- Corrected Vbat divider ratio and TMP236 formula against a real battery/thermometer.
- The EEPROM version-bump migration path (0x0001→0x0003) on an EEPROM that actually has
  old data written to it.

## wp3 — resolution and adaptation notes (2026-08-17)

Cherry-picked `f2646e4` (the REV A prototype's encoder+buzzer+UI-state-machine commit)
onto `wp2@83c4617`, dropped `880c218` (its CubeMX-regen-only companion, TIM1-era and
therefore moot), then hand-adapted for REV B hardware and a deliberately trimmed scope —
driven by direct user requirements, not just mechanical pin renames:

**Scope history:** the first pass at this (commits `e14e086`/`58afb60`/`015bb94`) trimmed
scope to "just evaluate the inputs... just to verify the code" per an early instruction in
the session, reverting `App/app_ui.c`/`.h` and the multi-screen `App/app_display.c`
compositor back to WP1-stub / single-screen. The user then clarified they wanted
`.workpackages/WP3_prompt.md`'s full original scope (commit after `015bb94`) — the UI state
machine and multi-screen display are now implemented, adapted for REV B rather than a
straight restore of `f2646e4`'s versions: `Services/svc_input.c` was narrowed to pure
polling (no beeping — `app_ui.c` now owns all buzzer triggering, contextually, avoiding a
double-beep between the two), `drv_encoder`'s `EncoderState`/`get_state()`/`clear()` API
from the prototype was **not** brought back (the 2026-08-17 decision earlier in this same
session — full quadrature, raw transition count — stands; `app_ui.c`'s `consume_detents()`
converts raw counts to clicks instead), button press events move through new
`g_system_state.encoder{1,2}_sw_press_event` latches (buttons are polled, not EXTI, on
REV B — see below), and `battery_cutoff_mv` (REV A-era field, no longer exists) maps to
today's `battery_critical_mv`. The buzzer stayed **single-tone at 2 kHz** (that decision
also stands) rather than the prototype's 1200/2400 Hz nav/confirm split — beeps differ only
in duration (20 ms nav / 40 ms confirm) to keep some of the original's tactile distinction.

**Hardware-driven changes beyond pin renames:**
- **Buzzer moved timers entirely**, not just pins: REV A prototype used TIM1_CH2/PB3; REV B
  has no TIM1 buzzer channel at all — buzzer is TIM3_CH4/PC9, and TIM3 was *also* VCOM's
  timer on the REV A design (now TIM6 on REV B, itself a WP1-era fix — see "Known critical
  bug" below). `HAL_App/hal_tim.c` rewritten around `htim3`/`TIM_CHANNEL_4` instead of
  `htim1`/`TIM_CHANNEL_2`; the ARR=(1MHz/freq)-1 math is unchanged (APB1 is still 64 MHz
  unprescaled on both revisions).
- **Buzzer tone: 2 kHz, single tone** (2026-08-17 user decision), not the prototype's
  1200 Hz nav / 2400 Hz confirm split — checked against the actual buzzer datasheet (Same
  Sky `CPT-9019A-SMT-TR`, piezo, externally driven, no internal oscillator): its frequency-
  response curve peaks loudest around ~5.5 kHz (~84 dB), with 2 kHz landing on a smaller
  secondary peak (~73 dB) — user chose 2 kHz anyway as less piercing. `BuzzerTone` enum
  trimmed to one `BUZZER_TONE_CLICK` member accordingly.
- **Encoder decode: full 4x quadrature, not A-edge-only.** The REV A prototype interrupted
  only on the A signal and read B's level to infer direction (`a == b` → CW). REV B's
  CubeMX config (`Core/Src/gpio.c`) fires `GPIO_MODE_IT_RISING_FALLING` on *all four* A/B
  pins, not just the A pins — confirmed intentional (`docs/pinout_migration_wp2-5.md`'s
  EXTI table), not an oversight to route around. `Drivers_App/drv_encoder.c` was rewritten
  around a standard Gray-code transition table driven from either pin's edge, evaluating
  every transition rather than only A's. Per 2026-08-17 user decision, this reports the
  **raw signed transition count** (`drv_encoder_get_count()`, unchanged original WP1-stub
  signature — the prototype's `EncoderState`/`get_state()`/`clear()` redesign was dropped),
  not a mechanical-detent count: this board's encoder part isn't confirmed to be 4
  transitions/detent, so that translation is explicitly left to a future UI/input layer.
  CW sign convention is a naming choice only, **not verified against real hardware rotation
  direction** — flagged in the table's own comment for hardware bring-up.
- **Encoder push switches (`ENC_1SW`/`ENC_2SW`) are polled, not EXTI, on REV B** — a genuine
  regression in interrupt capability, not a firmware choice: PA0/PC5 can't be routed to
  their own EXTI lines on this pinout (PA0 shares EXTI line 0 with PB0/`ENC_1B`, and
  `docs/pinout_migration_wp2-5.md` documents both switches were deliberately moved to
  `SYS_WKUP1`/`SYS_WKUP5` instead — see `pin_config.h`). `Services/svc_input.c` polls both
  every scheduler tick (RC-filtered + Schmitt-buffered, no further debounce needed) rather
  than registering EXTI callbacks the prototype's `drv_encoder.c` used to.
- **CubeMX regen done by hand, not via the GUI** (no STM32CubeMX/CubeIDE install found on
  this machine): `Core/Src/gpio.c`'s encoder pins already had `GPIO_MODE_IT_RISING_FALLING`
  and `Core/Src/tim.c` already had TIM3 CH4 PWM configured (apparently from earlier,
  undocumented work) — what was missing and had to be added directly was the NVIC enable
  for `EXTI0_1_IRQn`/`EXTI2_3_IRQn`/`EXTI4_15_IRQn` and their handler functions in
  `Core/Src/stm32g0xx_it.c`, following the same hand-edit-generated-files precedent WP2
  already established for ADC1/DMA. Separate commit `58afb60`.

**Reused as-is from the prototype** (already correct, no REV B-specific content):
`HAL_App/hal_gpio.c`'s EXTI dispatch mechanism (`HAL_GPIO_EXTI_Rising/Falling_Callback`
weak overrides forwarding to per-pin callbacks via `hal_gpio_exti_register`) and
`Drivers_App/drv_buzzer.c`'s non-blocking beep/update pattern (wrap-safe signed millisecond
countdown, no `HAL_Delay` blocking) — both pin-agnostic, ported unchanged.

**Build-verified (2026-08-17)** — `arm-none-eabi-gcc` 14.2.1, CMake 4.4.2, and Ninja 1.13.2
installed via `winget` on this machine (none were present before; the checked-in `build/`
directory had been a stale artifact from a different machine/user,
`C:/Users/lfaes/...` in its CMakeCache — removed and reconfigured fresh). Every file had
already been manually re-read for type/include/logic correctness before the toolchain
existed to check that directly; the subsequent clean build (zero warnings under
`-Wall -Wextra -Werror`, all 207 objects, links to a valid ELF) confirms that review was
accurate. See the Status table above for exact figures.

**Still outstanding, beyond the "not on real hardware" gap:**
- Quadrature sign convention (CW = +1) unverified against real hardware rotation.
- Whether this encoder part is actually 4 raw transitions per mechanical detent — needed
  once a UI layer wants to translate `drv_encoder_get_count()` into "clicks."
- Buzzer audibility/loudness at 2 kHz on the real board, and whether `ENC_1SW`/`ENC_2SW`
  polling (rather than interrupt) feels responsive enough for real button presses and screen
  navigation.
- `encoder_counts_per_detent` (EEPROM-backed `DeviceSettings` field, `DEFAULT_ENCODER_COUNTS_PER_DETENT`=4
  in `config.h`) is still an assumption — confirm against the real encoder part; correcting
  it now only needs a settings write, not a reflash (see "Code review" below).
- EEPROM settings save-on-confirm (`svc_storage_save_settings()` from the SETTINGS screen)
  hasn't been exercised against real EEPROM hardware in this pass — inherits the same
  "not yet flashed" caveat as everything else here.

## Code review (2026-08-17)

`/code-review` at high effort: 8 finder angles (3 correctness, 3 cleanup, altitude,
conventions) against `wp2...HEAD`, ~25 raw candidates, 12 sent through 1-vote verification,
10 survived (2 refuted: a naming/header-guard finding turned out to match 100% of this
codebase's pre-existing style, not a regression). All 10 fixed and re-verified via a clean
rebuild — see commit `8365768`. Highlights:

- **Spurious button-press event on Standby wake** — `svc_input_init()` seeded button state
  as "not pressed" unconditionally, but `ENC_1SW`/`ENC_2SW` are the Standby wake sources;
  waking by holding one down made the first poll see a false press edge. Fixed by seeding
  from the actual pin level at init.
- **Re-entrant `app_scheduler_init()` bug** — `commit_edit()` called it from inside
  `task_ui`, itself mid-iteration of `app_scheduler_run()`'s own loop; an unsigned-wraparound
  in the reset logic spuriously re-fired not-yet-run tasks the same tick and delayed every
  task's next run (including battery monitoring). Fixed with a new
  `app_scheduler_reload_periods()` that doesn't touch `last_run_ms`.
- Frozen STATUS-screen uptime, a boot-time encoder-state race, a dropped instance bounds
  check, `ENCODER_COUNTS_PER_DETENT` moved to EEPROM, a duplicated settings-read switch, a
  6x-duplicated elapsed-time idiom now factored into `hal_systick_elapsed_ms()`, an unlogged
  EEPROM-save failure, and an O(16)→O(1) EXTI dispatch fix — see commit `8365768`'s message
  for the full list.

**Second pass (2026-08-17, commit `3ad510c`), reviewing the fix commit itself:** 8 finder
angles launched; A/B/C/F/H hit an API session limit mid-run, so this pass covers D (reuse),
E (simplification), G (altitude) plus direct manual verification — not full 8-angle coverage.
9 findings, 8 fixed: `settings_save_failed` was set but never displayed (now rendered on the
SETTINGS screen); `app_scheduler_init()` remained re-entrancy-unsafe alongside the new safe
function, protected only by a comment (collapsed into one self-protecting design, guarded by
a static `s_booted` flag); the EXTI bitmask-to-line bit trick was duplicated between
`hal_gpio.c` and `drv_encoder.c` (consolidated into `hal_gpio_pin_to_line()`);
`drv_encoder_init()` branched on encoder instance twice (consolidated to once);
`encoder_counts_per_detent` was re-validated every UI tick instead of once at EEPROM load
(moved to `svc_storage_init()`); per-setting label/unit/range were still scattered across 5
constructs in 2 files even after the first pass's read-switch consolidation (unified into one
`UiSettingMeta` table). One finding (`Error_Handler()` for an invalid encoder instance) was
considered and **not applied** — no precedent anywhere in this codebase for a lower layer
calling that Core-layer function; kept the existing `DrvStatus`-based error propagation.
One finding (bump `EEPROM_SETTINGS_VERSION` reseeds *all* settings, not just the new field)
was confirmed real but is pre-existing WP2-era project debt this bump exercises again, not a
new regression — fixed a stale comment claiming otherwise and documented the risk rather than
building a field-preserving migration system (no hardware calibrated/flashed yet, so no real
data is at risk today). Applying the elapsed-time-helper suggestion to the scheduler's own
`app_scheduler_run()` loop was caught as a self-inflicted regression during implementation
(would desync each task's due-check from its `last_run_ms` stamp) and reverted with an
explanatory comment. Build-verified clean after fixes.

**EEPROM page split (2026-08-17, commit `30b14e8`) — actually fixes the version-bump finding
above, instead of just documenting it as accepted risk.** User's proposal: since the 24LC256
has 32KB and settings use under 1KB, give each subsystem (Scheduler/Timing, Battery, TMP236,
LM35, Encoder) its own EEPROM page with its own magic/version/CRC header, instead of one
version number covering the whole `DeviceSettings` struct. `g_device_settings` stays one flat
struct in RAM — no consumer code changed — only `Services/svc_storage.c`'s persistence layer
changed internally, via a `SettingsSection` table (address + version + `offsetof`/`sizeof`
byte range per subsystem) driving both load and save. `EEPROM_CALIBRATION_ADDR` moved
`0x0100`→`0x0500` to make room; addresses `0x0000`-`0x0400` now hold the five settings pages.
Removed the vestigial `checksum` struct field (set but never read — real integrity checking
was always the header's separate CRC). The "still outstanding" `encoder_counts_per_detent`
bullet above is now more precisely scoped: its own page can change layout/version without
affecting battery or TMP236 calibration.

**Third review pass (2026-08-17, commit `05070d7`) on the page-split commit itself found a
real data-corruption bug, fixed:** `load_section()` wrote each page's EEPROM read result
directly into the live `DeviceSettings` struct *before* validating its CRC/version. On a
mismatch, the corrupted/mismatched bytes had already overwritten the good default
`svc_storage_init()` seeded — and the reseed step then computed a *fresh* CRC over that
corruption and wrote it back, making it pass validation permanently on every subsequent boot
instead of being caught and replaced. Fixed by reading into a local buffer first and only
committing to the live struct after CRC and version both pass — now genuinely backed by
`_Static_assert` checks (committed in `Services/svc_storage.c`, not just run once in a
throwaway file as the doc previously — inaccurately — claimed). Also fixed: `settings_save_failed`
only reflected whether a save was *queued*, not whether the async 5-page sequence actually
*completed* — a real mid-sequence failure went completely unreported; `svc_storage_update()`
now sets/clears it at the true completion point instead of `commit_edit()`'s optimistic
synchronous clear. Matching zero-guards added for `vbat_scale_den`/`tmp236_seg1_den`/
`tmp236_seg2_den` (previously only the encoder field had one).

**Consciously left as accepted scope limitations, not code-fixed** (each would need
disproportionate new infrastructure for a condition that requires an actual EEPROM/I2C
hardware fault, on a device that's never been flashed):
- The 5-page settings save is not atomic across pages — a mid-sequence failure (after 3
  retries) can leave some pages holding new values and others stale. `settings_save_failed`
  now correctly reports this happened, but doesn't undo the partial write.
- Boot-time worst case grew from one ~200 ms blocking reseed write to up to five (~1 s) if
  every page independently fails against a genuinely stuck I2C bus.
- Old EEPROM contents written under the pre-split single-page layout (settings at `0x0000`,
  calibration at `0x0100`) are reinterpreted under the new per-page address map with only the
  magic-byte+CRC check as a (coincidental, not deliberate) safeguard against cross-layout
  garbage. Moot today — no hardware has been flashed under the old layout.

## Known critical bug found — RESOLVED (2026-08-15, commit `251501d`)

**Previously:** `HAL_App/hal_tim.c`'s `hal_tim_vcom_start()` still drove `TIM3_CH1` hardware
PWM for the display VCOM signal. REV B removed VCOM's hardware timer channel entirely —
`PD3` is a plain `GPIO_Output`, and `TIM3_CH4` is committed to the buzzer. This compiled fine
but did **nothing** to the actual VCOM pin. Per `CLAUDE.md`, an un-toggled VCOM **permanently
damages the LS027B7DH01**. This was WP1 code (predates all of wp2–5, already on `master`), so
it wasn't folded into the wp2–5 rebase itself — but was fixed directly on `master` in a
separate pass, alongside the rest of WP1's REV B port:

- VCOM now toggled manually in a TIM6 period-elapsed ISR (the approach already spec'd in
  `docs/cubemx_configuration_checklist.md` §2), tuned to 5 Hz (`TIM6.Period` 33→99) — the
  original value landed at ~14.7 Hz, above the display datasheet's 1–10 Hz VCOM spec.
- Display SPI moved from `hspi1`/SPI1 (REV A pin, wrong peripheral on REV B) to `hspi2`/SPI2
  (PD1/PD4), matching REV B's actual wiring.
- `3V3_EN` polarity fixed (REV B is active-LOW on PC6, `hal_gpio_init()` had it inverted and
  would have left the rail permanently off) and the previously-unasserted `5V_EN` (PC7) rail
  — which the display depends on — is now enabled at boot, with a 20 ms settle delay before
  any downstream peripheral use.
- `Config/pin_config.h` fully repointed to REV B pin numbers (display CS/ON/SCK/MOSI/VCOM,
  LEDs on PB13/PB14).

**Build-verified only** (clean compile, zero warnings under `-Wall -Wextra -Werror`, linker
map confirms all HAL callback overrides resolve correctly) — **not yet flashed to hardware.**
Still outstanding: scope PD3 to confirm the 5 Hz toggle, confirm rail sequencing, and visually
confirm "Hello World" + LED heartbeat render on real REV B hardware.

---

## wp4 — resolution and adaptation notes (2026-08-17)

Unlike `wp2`/`wp3`, this was **not** a cherry-pick. `d1423b5` (the WP4 feature commit) and
`48e8562` (VID/PID) diff against the old REV-A prototype's `app_ui.c`/`app_display.c`/
`app_scheduler.c` — files WP3 rewrote wholesale (unified `UiSettingMeta` table instead of
parallel switches, multi-screen compositor with change-detection, safe
`app_scheduler_reload_periods()`). A cherry-pick would have conflicted on nearly every hunk
in those three files. Ported file-by-file onto `wp3@2d5fc7e` instead, applying the same
"REV B hardware-driven changes, not mechanical renames" standard as `wp2`/`wp3`:

**REV B's WP1 port had already done half the USB work, unknowingly.** `Core/Src/main.c`
already called a standalone `MX_USB_DRD_FS_PCD_Init()`, and `Drivers/STM32G0xx_HAL_Driver/`
already carried the full PCD/LL-USB driver files (`stm32g0xx_hal_pcd.c`, `_ex.c`,
`stm32g0xx_ll_usb.c`) and `cmake/stm32cubemx/CMakeLists.txt` already built them — none of
that existed on the old pre-REV-B history until WP4's own `ff054fb` added it. This also
means **CLAUDE.md's Open Item 3 (non-standard USB pins) is moot on REV B**: the pinout CSV
puts USB on the standard PA11 (`USB_DM`)/PA12 (`USB_DP`) pins the WP4 code already assumed —
`docs/pinout_migration_wp2-5.md` §11 line 63 confirms this was already checked during the
REV B pinout survey. What was still missing was the USB **Device middleware** layer
(`Middlewares/ST/STM32_USB_Device_Library/`, `USB_Device/App+Target/`) — never added on REV
B, and `ff054fb` (the branch that did add it) is REV-A/PA9-PC6-era and was dropped per the
existing plan.

**Middleware added by hand** (no CubeMX GUI in this environment — same constraint as every
other WP here):
- `Middlewares/ST/STM32_USB_Device_Library/` (Core + Class/CustomHID) is vendor code with no
  board-specific content — ported verbatim from `ff054fb`.
- `USB_Device/App/` + `USB_Device/Target/` needed real adaptation, not just a copy:
  - **Duplicate-symbol conflict discovered and resolved:** `Core/Src/usb_drd_fs.c` (REV B's
    existing standalone peripheral init) and `USB_Device/Target/usbd_conf.c` (the Device
    middleware's own bridge layer) both declare the global `hpcd_USB_DRD_FS` handle and both
    define `HAL_PCD_MspInit`/`MspDeInit` — a link error if both are built. This is the same
    "CubeMX regen supersedes an earlier IP-only config" situation `CLAUDE.md` §9.2 describes;
    resolved by deleting `usb_drd_fs.c`/`.h` (its `HAL_PCD_MspInit` didn't even enable the
    NVIC interrupt — an incomplete stub `usbd_conf.c`'s version fully supersedes) and
    replacing `main()`'s `MX_USB_DRD_FS_PCD_Init()` call with `MX_USB_Device_Init()`.
  - **Report descriptor was never actually written in the old history either** — WP4's own
    commit message flagged it as a required manual step (`hal_usb.c`'s banner: "you must
    inject two USER CODE blocks"), and `ff054fb`'s version was still the 2-byte
    `usbd_customhid.h` placeholder. Wrote a real 29-byte vendor-defined HID descriptor (one
    64-byte opaque array each direction — `svc_api.c` owns the actual framing inside it).
  - **`CUSTOM_HID_EPIN/EPOUT_SIZE` and `USBD_CUSTOMHID_OUTREPORT_BUF_SIZE` were still the ST
    class template's 2-byte defaults** (sized for tiny reports like a keyboard LED byte) —
    bumped to `USB_HID_REPORT_SIZE` (64) via `usbd_conf.h`, which `usbd_customhid.h`'s own
    include chain (`usbd_ioreq.h` → `usbd_def.h` → `usbd_conf.h`) pulls in before its
    `#ifndef` guards run, so the override lands correctly regardless of which file triggers
    the include first.
  - **`CUSTOM_HID_OutEvent_FS`'s callback signature only carries 2 bytes**
    (`event_idx`/`state` — the class was designed around single-byte reports), not the full
    64-byte report. The real received bytes sit in the private class handle's `Report_buf[]`,
    reached via `hUsbDeviceFS.pClassDataCmsit[hUsbDeviceFS.classId]` — that's what actually
    gets forwarded into `hal_usb_on_rx()`.
  - `USBD_CUSTOM_HID_SendReport_FS` (what the original `hal_usb.c` called) is `static` inside
    `usbd_custom_hid_if.c` and not linkable from `HAL_App/` — called `USBD_CUSTOM_HID_SendReport`
    directly with `&hUsbDeviceFS` instead, matching the fix the old history's own `ff054fb`
    commit message documented for the same problem.
  - VID/PID/manufacturer/product strings pulled from `Config/config.h`'s existing
    `USB_VID`/`USB_PID`/`USB_MANUFACTURER_STR`/`USB_PRODUCT_STR` instead of the class
    template's hardcoded STMicroelectronics placeholders. Serial string descriptor left on
    the template's own MCU-unique-ID-derived generator (more robust than a fixed string).
- `USB_UCPD1_2_IRQHandler` added to `stm32g0xx_it.c`/`.h`, dispatching to
  `HAL_PCD_IRQHandler(&hpcd_USB_DRD_FS)` — verified byte-for-byte against
  `startup_stm32g0b1xx.s`'s vector table entry (STM32G0B1's USB, UCPD1, and UCPD2 all share
  one NVIC line).

**Application layer, ported with adaptation, not verbatim:**
- `Services/svc_api.c`/`svc_measurement.c`/`Math/math_settling.c`: no REV B-specific content
  (pure protocol/math logic), ported unchanged except one real bug fix — `SET_SETTINGS`
  called `app_scheduler_init()`, which is boot-only and no-ops on every call after the first
  (a WP3 code-review fix, see above) — changed to `app_scheduler_reload_periods()`, the
  function that fix introduced specifically for this kind of runtime-reload call site.
- `App/app_scheduler.c`: `task_usb`/`task_api`/`task_measurement` added onto the existing
  task table and `app_scheduler_reload_periods()` (not the old history's now-superseded
  `app_scheduler_init()`-only version).
- `App/app_display.c`: STR/RAW/SGL top-bar badges and the MEASURING overlay ported onto the
  current multi-screen compositor. Added `MeasurementState`/`ApiMode` tracking to
  `DisplaySnapshot`'s change-detection struct — didn't exist when this was first written
  against the old prototype's redraw-every-tick display code, and without it the overlay
  could fail to appear/disappear promptly since neither field was otherwise watched.
- `App/app_ui.c`: **not touched.** The old history's only WP4 addition there was the
  "Reset BLE" settings action (`ble_configured = false`) — out of scope, see below.

**Deliberately not ported:**
- `EEPROM_SETTINGS_VERSION` → `0x0002` and `DeviceSettings.ble_configured`: the old history's
  monolithic single-version EEPROM scheme these belong to no longer exists — `wp3` replaced
  it with independently-versioned per-subsystem pages (`Services/svc_storage.c`'s
  `SettingsSection` table). `ble_configured` has no purpose until BLE actually lands in
  `wp5`; adding it now would mean inventing a "BLE settings" page for a field nothing reads
  yet. Bring it in during the `wp5` rebase instead, as its own page following the established
  `SettingsSection`/`_Static_assert` pattern.
- WP1.5 debug UART (`db251c2`, found orphaned on branch `claude/lucid-thompson-923f35`,
  never merged anywhere): explicit 2026-08-17 user decision to skip it rather than adapt and
  splice it in ahead of `wp4` — USART3 becomes `svc_api`'s third `ApiTransport` directly, no
  separate text-logging channel first. `svc_api.h`'s `ApiTransport` enum currently has only
  `API_TRANSPORT_USB`/`API_TRANSPORT_BLE`; a `API_TRANSPORT_UART` (or similar) member and a
  `svc_uart` adapter mirroring `svc_usb.c` would be the shape of that future addition.

**Build-verified (2026-08-17)** — zero warnings under `-Wall -Wextra -Werror`, all 213/214
objects (213 compiled + link), FLASH 113 KB (21.6%), RAM 33.2 KB (22.5%). `FW_VERSION`
bumped to 0.4.0. **Not yet flash-tested** — nothing here has touched real silicon, same
caveat as every other WP in this document.

**Still outstanding, beyond "not on real hardware":**
- The Custom HID report descriptor, `CUSTOM_HID_EPIN/EPOUT_SIZE`, and
  `USBD_CUSTOMHID_OUTREPORT_BUF_SIZE` overrides are new content (not carried over from any
  prior working state) — worth a host-side smoke test (`tools/test_usb.py`, itself unchanged
  from the old history, not yet re-verified against this port) before trusting the framing
  end-to-end.
- Sensors aren't fused into `g_system_state`'s tilt fields yet (pre-dates WP4 — same gap the
  old history had), so `svc_measurement`/`svc_api`'s STREAM/RAW_STREAM/SINGLE payloads carry
  real timestamp/battery/status data but zero tilt values until a future WP wires that up.
- `[ENC2 push] Cancel` on the MEASURING overlay is a display-only hint — the old history
  never wired `svc_measurement_cancel()` into `App/app_ui.c`'s encoder-2-push handler, and
  this port didn't add it either (out of scope: no app_ui.c changes, see above).

**Update (2026-08-17, later same day): reconciled against a real CubeMX regen.** The user
had STM32CubeMX installed after all and walked through configuring `USB_DEVICE` (Custom
HID class, VID/PID/strings, `USBD_CUSTOM_HID_REPORT_DESC_SIZE`=29,
`USBD_CUSTOMHID_OUTREPORT_BUF_SIZE`=64) in the actual GUI, then regenerated — closing the
"never updated" gap noted above. Findings from reconciling:
- Most hand-written content in `USB_Device/App+Target/` survived the regen completely
  untouched — CubeMX's merge respects USER CODE markers, and the report descriptor bytes,
  `CUSTOM_HID_EPIN/EPOUT_SIZE` override, and `hal_usb_on_rx()` forwarding all lived inside
  or adjacent to them.
- `usbd_desc.c`'s VID/PID/manufacturer/product are now **CubeMX-owned hardcoded literals**
  (correct values, matching `Config/config.h`'s `USB_VID`/`USB_PID`/etc.) rather than reading
  from `config.h` — that indirection lived outside any USER CODE marker, so it didn't survive.
  Accepted rather than re-applied: fighting CubeMX's own regen output on every future
  regeneration isn't worth it for cosmetic string literals. **Not mechanically linked** to
  `config.h` anymore — if VID/PID/strings ever change, both `usbd_desc.c` (via CubeMX's GUI)
  and `config.h` need updating by hand, since `svc_api.c`'s `GET_IDENTITY` response still
  reads the `config.h` macros.
- CubeMX restructured `cmake/stm32cubemx/CMakeLists.txt` more cleanly than the hand-edit
  had — a proper `USB_Device_Library` OBJECT target instead of appending into
  `STM32_Drivers_Src`. Kept that structure; had to re-add `Config/` and `HAL_App/` to
  `MX_Include_Dirs`, which CubeMX has no awareness of and dropped.
- **Real regression caught by this regen, unrelated to USB:** `Core/Src/gpio.c`'s
  `MX_GPIO_Init()` lost the `EXTI0_1_IRQn`/`EXTI2_3_IRQn`/`EXTI4_15_IRQn` NVIC enable calls
  for the WP3 encoder pins. The `.ioc` tracks each pin's EXTI *trigger mode* but never
  tracked the NVIC *enable* checkbox for those three lines (that was only ever hand-added
  straight into `gpio.c` back in WP3, never reflected in the `.ioc`) — so this entirely
  unrelated USB regen silently deleted it. Without the fix, encoder rotation would have
  produced zero interrupts despite every other piece of WP3's encoder code being intact and
  unchanged — exactly the kind of silent, hard-to-notice breakage `CLAUDE.md` §9.2's "review
  the full diff carefully" instruction exists to catch. Fixed by moving the three
  `HAL_NVIC_SetPriority`/`EnableIRQ` pairs into `main.c`'s `USER CODE BEGIN 2` block (proven
  regen-safe by this very regen preserving everything else there) instead of leaving them in
  CubeMX-owned `gpio.c` territory a second time. **Any future CubeMX regen should be diffed
  file-by-file before trusting it**, the same way this one was — a clean build alone would
  not have caught this, since the code compiles fine either way; only the encoder physically
  stops responding.

**Update (2026-08-18): a code review pass, then a second real CubeMX regen, closed this out
properly.** A `/code-review` pass on the full wp3...wp4 diff (9 finder angles, independent
second-pass verification on every surviving candidate) found 9 confirmed bugs, including the
worst-case version of the risk flagged above: the CubeMX regen had actually deleted the
`EXTI0_1/2_3/4_15_IRQHandler` *function bodies* in `stm32g0xx_it.c`, not just the NVIC-enable
calls — meaning the `main.c` workaround above made the hard-hang **more likely** to trigger
(NVIC now enabled, no handler behind it) rather than fixing anything. Also found: the USB RX
callback was wiped by `hal_usb_init()` immediately after being registered (USB could send but
never receive), the OUT report buffer was undersized against what the code reads (an
out-of-bounds struct read), the HID report descriptor had reverted to a broken placeholder,
and `SET_SETTINGS`/`SET_CALIBRATION`/`SET_ZERO` silently ignored EEPROM write failures and
skipped input validation. All fixed — see commit `eecf934`.

Separately, the user then regenerated again from a different machine, this time having
**ticked the NVIC enable checkboxes for the three EXTI lines in the CubeMX GUI itself**.
`WylerLeveltronic.ioc` now carries `NVIC.EXTI0_1_IRQn`/`EXTI2_3_IRQn`/`EXTI4_15_IRQn`, and
`gpio.c` generates the enable calls on its own — **this closes the gap for good**; it's no
longer a hand-maintained workaround that a future unrelated regen can silently drop. The
redundant `main.c` block from the first fix was removed accordingly. Two things still don't
survive a regen and were re-applied a second time (confirmed lost identically on both
regens, since the GUI has no field for the first and the second lives in an array
initializer outside any USER CODE marker): `CUSTOM_HID_EPIN_SIZE`/`CUSTOM_HID_EPOUT_SIZE` in
`usbd_conf.h`, and the report descriptor bytes in `usbd_custom_hid_if.c`. Both are now called
out with an explicit "re-verify after every future regen" comment at their definition site.

Build-verified after both passes: zero warnings, 214/214 objects, FLASH 113.2 KB (21.6%),
RAM 33.2 KB (22.5%). Still not flash-tested on real hardware.

**Update (2026-08-18): a second full `/code-review` pass, requested explicitly before
pushing.** Same method (9 finder angles on the full `wp3...HEAD` diff, independent
re-verification on every candidate) run fresh against the state after the first review pass
and both regens. No crash-level bugs this time — everything from the first pass held up —
but 8 real issues survived undetected until this second look, all fixed (commit `c7665dc`):
- `svc_storage_update()`'s retry-exhausted branch only escalated `settings_save_failed` for
  settings saves, not calibration saves — a `SET_CALIBRATION` that ACKs on synchronous
  queue-success could still fail asynchronously with nothing surfacing it. Now symmetric for
  both kinds of save.
- `svc_api.c`'s `SET_ZERO`/`SET_CALIBRATION`/`SET_SETTINGS` didn't check
  `svc_storage_is_busy()` before saving — ordinary contention with an in-flight local-UI save
  produced a false "SAVE FAILED" and NACK. Now checked first; busy NACKs without escalating.
- `svc_storage_validate_settings()`'s comment claimed to cover "every EEPROM-backed divisor"
  but only guarded 4 of 6 — `filter_cutoff_hz_den`/`lm35_scale_mv_per_c` (unconsumed today,
  future WP5/LM35-driver fields) had no guard. Added, so the comment is now actually true.
- The measuring overlay's "[ENC2 push] Cancel" hint had no wiring behind it — encoder-2-press
  never called `svc_measurement_cancel()`, only reachable via a connected host. Wired in.
- `svc_usb.c`'s `send_via_usb()`/`svc_usb_send()` silently discarded USB send failures
  (CLAUDE.md 7.6). Added `g_system_state.usb_tx_dropped_count` to at least make drops
  observable (no retry queue — that's a bigger feature). `svc_usb_send()` itself was dead
  code (zero callers) — deleted.
- The three `SET_*` handlers' duplicated save-result-and-ack/nack logic factored into one
  `handle_save_result()` helper.
- `CUSTOM_HID_EPIN_SIZE`/`EPOUT_SIZE` moved into `usbd_conf.h`'s `USER CODE BEGIN/END
  INCLUDE` block — the comment-only mitigation from the first regen reconciliation had
  already failed to prevent silent loss on a *second* regen; the marker block is the one
  place in this file confirmed to survive both. The report descriptor in
  `usbd_custom_hid_if.c` has no equivalent regen-safe home (its content lives in an array
  initializer, not a marker) — still comment-only, still needs a human re-check after any
  future regen.
- `hal_usb_send()` no longer unconditionally re-zeros a full 64-byte buffer the caller
  already zero-padded.

Build-verified after this pass too: zero warnings, 214/214 objects, FLASH 113.4 KB (21.6%),
RAM 33.2 KB (22.5%). Still not flash-tested on real hardware — everything here is
logic-level review, not a substitute for real silicon.

---

## Resuming this work

1. Read this file and the two docs linked at the top.
2. **`wp3` is done, build-verified, and through three code-review passes** (see "wp3 —
   resolution and adaptation notes" and "Code review" above — the third pass fixed a real
   EEPROM data-corruption bug in the per-page storage redesign). `wp3` HEAD is `8f9d3e8`.
   Still needs real-hardware flash testing — nothing here has touched real silicon yet.
3. **`wp4` is done, build-verified, and code-reviewed** (see "wp4 — resolution and adaptation
   notes" above — 9 findings from a `/code-review` pass, 8 fixed, including a critical
   regen-induced encoder hard-hang; 1 skipped as low-impact), plus a second pass after both
   regens landed (8 more findings, all fixed, no crash-level bugs this time). `wp4` HEAD is
   `c7665dc`. Still needs real-hardware flash testing — nothing here has touched real
   silicon yet.
4. Move to `wp5`: `git checkout wp5 && git reset --hard wp4 && git cherry-pick 1662959 73aa050`
   (decide on `81a9643`, drop `4bc4f16`) — `wp4` here means its current tip (`c7665dc`).
   `1662959`'s own commit message flags a real pin conflict between its RN4871 UART choice
   and WP3's *old* encoder pins (PA2/PA3) — REV B's actual WP3 encoders are on
   PC4/PB0/PB1/PB2 now, so re-verify `73aa050`'s PD4/PD5/PD6 relocation is still free and
   correct on the current netlist rather than assuming the old conflict (or its fix) still
   applies as-is. This is also where `DeviceSettings.ble_configured` (deliberately skipped in
   `wp4`, see above) should be added, as its own `SettingsSection` page. Resolve conflicts,
   apply the same 6-item checklist, grep for stale pin names, build and verify.
5. Stop after `wp5` builds clean. Do not merge into `master` — that's a separate review step.
