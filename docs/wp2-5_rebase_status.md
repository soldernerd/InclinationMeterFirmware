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

| Branch | State | Base | Feature commits kept | Regen commit dropped |
|---|---|---|---|---|
| `wp2` | **Rebased, committed, force-pushed to origin** | `master@e6a756d` | `2ef77cf` → new `f996ff9` | `cf855c6` |
| `wp3` | Not started | `wp2` (rebased) | `f2646e4` | `880c218` |
| `wp4` | Not started | `wp3` (rebased) | `d1423b5`, `48e8562` | `ff054fb` |
| `wp5` | Not started | `wp4` (rebased) | `1662959`, `73aa050`, decide on `81a9643` (docs-only, likely stale post-REV-B, review before keeping) | `4bc4f16` |

**Build verification:** no ARM toolchain (`arm-none-eabi-gcc` / `cmake` / `ninja`) was
reachable in the environment this rebase was done in. `wp2` was checked by manual/static
review only — every changed macro traced from definition to call site, `grep` swept for
leftover references to old pin names. **Not actually compiled.** Run a real build before
trusting any rebased branch.

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
- **TMP236 vs. LM35** — REV B's on-board temp sensor changed parts. `LM35_SCALE`'s formula
  was left in place (no datasheet available to re-derive it correctly) but flagged loudly in
  `pin_config.h` and `drv_lm35.c` as unverified. Do not trust absolute temperature readings
  until this is re-derived.

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

## Resuming this work

1. Read this file and the two docs linked at the top.
2. `git checkout wp3 && git reset --hard wp2 && git cherry-pick f2646e4` (drop `880c218`).
3. Resolve conflicts, apply the same 6-item checklist, grep for stale pin names.
4. Build and verify before moving to `wp4`. Update this file's status table and add a
   `wp3` section mirroring the `wp2` one above.
5. Repeat for `wp4` (base `wp3`, commits `d1423b5` + `48e8562`, drop `ff054fb`) and `wp5`
   (base `wp4`, commits `1662959` + `73aa050` + decide on `81a9643`, drop `4bc4f16`).
6. Stop after `wp5` builds clean. Do not merge into `master` — that's a separate review step.
