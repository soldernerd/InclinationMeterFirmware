# Pinout Migration Survey: WP2–WP5 vs. New Hardware Pinout

**Purpose:** cross-reference what the WP2–WP5 feature branches actually require in code
against `STM32G0B1RET6_Pinout.csv` (the new, already-ordered hardware). Survey only — no
code, `.ioc`, or rebase changes were made.

**Method:** for each branch, the CubeMX-regen-only commits were excluded; requirements
were extracted from the feature commits themselves plus the resulting `Config/pin_config.h`
and driver/service source (`Drivers_App/`, `HAL_App/`, `Services/`) at each branch tip —
not from what a particular old `.ioc` happened to generate. Branch chain confirmed via
`git merge-base`: `wp2` branches from `master`@`WP1`, `wp3` from `wp2`, `wp4` from `wp3`,
`wp5` from `wp4` (each is cumulative, not independent).

**Naming note:** these git branches use their own WP numbering (`wp2`=Power/ADC/EEPROM/
Battery, `wp3`=Encoders/Buzzer/UI, `wp4`=USB HID, `wp5`=BLE) which does **not** match the
WP2–WP6 definitions in `CLAUDE.md` §10 (there, WP2=SCL3300, WP3=PCAP04, WP4=BLE+USB,
WP5=Sensor Fusion+UI, WP6=Power Management). See Open Questions item 14.

---

## WP2 — Power, ADC, Temperature, EEPROM, Battery
*(feature commit `2ef77cf`, CubeMX regen `cf855c6` excluded)*

| Signal / Function | Old pin/peripheral (branch) | New pin/peripheral (CSV) | Notes |
|---|---|---|---|
| Power rail enable | PC0, GPIO Output, active-**HIGH** (`LDO_EN`) | PC6, GPIO Output, active-**LOW** (`!3V3_EN!`) | **Polarity inverted, confirmed by user** — not just moved. `main.c` asserts this pin before `HAL_Init()` via LL inline calls in the boot sequence — a naive rename without flipping the logic leaves the 3V3 rail permanently mis-driven. See Open Questions #6. |
| **In scope for WP2 port:** ±5V supply enable | *(no equivalent in any WP2–WP5 branch)* | PC7, GPIO Output, active-HIGH (`5V_EN`, "Low=off, High=on") | Confirmed by user as a real cross-subsystem power-sequencing requirement: the ±5V rail must be enabled whenever the display, buzzer, on-board or external temp sensor, or the DAC/ADC front end (Open Questions #2) is in use. The enable/disable *control* naturally lives in WP2's power module alongside `3V3_EN`, but WP2's own consumers of it are the two temp sensors below — see Open Questions #7 for the cross-WP sequencing implications (display is WP1, DAC/ADC is a future WP). |
| Status LEDs | PC1 (`LED_PWR`, green) / PC2 (`LED_STS`, blue), active-HIGH | **PB14** (`LED_PWR`) / **PB13** (`LED_STATUS`), active-HIGH ("High=on, low=off") | **RESOLVED — polarity confirmed unchanged.** `LED_STS`→`LED_STATUS` rename already anticipated by the `chore(hw)` rev-B commit on branch `claude/lucid-thompson-923f35` (**correction:** an earlier version of this doc misattributed that commit to `master` — `master` is WP1-only). **Pin update:** originally landed on PB11/PB12, but a hardware bug forced a rework — see the row below — displacing both LEDs to PB13/PB14. See `docs/cubemx_configuration_checklist.md` §0 for the full account. |
| Battery voltage sense (ADC) | PB0, `ADC_IN8`, divider 100k/68k (`VBAT_DIV_HIGH_K`=100, `VBAT_DIV_LOW_K`=68, ratio 68/168≈0.405) | **PB11**, Analog input (`BATTERY_SENSE`), divider "100k/68k = 0.4047..." per CSV | **RESOLVED — same divider values**, but **pin corrected after a hardware mistake.** The CSV originally put this on PA15, which turned out not to be a real ADC1-capable pin — CubeMX's "Analog" GPIO mode is selectable on any pin and doesn't guarantee an actual ADC channel behind it. Caught after the prototype board was already fabricated; fixed via manual rework to PB11, which displaced `LED_STATUS` to PB13 (see row above). Full account in `docs/cubemx_configuration_checklist.md` §0. Divider constants (`VBAT_DIV_HIGH_K`/`LOW_K`/`VBAT_SCALE_NUM`/`DEN`) are unaffected by the pin change. |
| On-board temp sense (LM35, ADC) | PB1, `ADC_IN9` | **PB12**, Analog input (`TEMP_SENSE`) | Same hardware mistake as `BATTERY_SENSE` above — originally PC8, not a real ADC1-capable pin, moved to PB12 (displacing `LED_PWR` to PB14). `LM35_SCALE` assumes VREFBUF at 2.048V/12-bit — re-derive if the reference config differs (see the VREFINT/VREFBUF row below). **Requires `5V_EN` asserted before reading** (user-confirmed dependency) — old code had no such precondition, since the old board had no 5V rail to sequence. |
| **New:** external temp sense | *(no equivalent)* | **PA3**, Analog input (`TEMP_SENSE_EXT`) | Same hardware mistake as above — originally PA10, not a real ADC1-capable pin, moved to PA3 (displacing `CHARGE_SENSE` to PC2, see row below). Not present in any WP2–WP5 branch. New ADC channel — needs new driver/service code, not a rename. **Also requires `5V_EN` asserted before reading**, same as on-board temp sense above. |
| Charge status sense | PA9, GPIO Input, active-LOW, pull-up (`CHG_SENSE`) | **PC2**, GPIO Input (`CHARGE_SENSE`) | Pin moves twice: first to PA3, then displaced again to PC2 to make room for `TEMP_SENSE_EXT` once the PA10 ADC mistake was discovered (see rows above and `docs/cubemx_configuration_checklist.md` §0). Old code treats it as TP4056 open-drain active-low with pull-up (`svc_battery.c`) — confirm same behavior and re-apply the pull-up in the new CubeMX config. |
| USB VBUS present sense | PA10, GPIO Input, active-HIGH (`VBUS_SENSE`) | PA2, GPIO Input, `SYS_WKUP4` | Pin moves and gains Standby wake-up capability. Old code (`hal_usb.c`, `svc_battery.c`) only polls it as plain GPIO — no WP branch implements Standby entry/wake yet, so this is new capability, not just a pin swap. |
| **In scope for WP2 port:** charge enable/disable control | *(no equivalent — old code only ever sensed charge state, never controlled it)* | PD6, GPIO Output, active-HIGH (`CHARGE_EN`, "High=charge, low=don't charge") | Confirmed by user as needed for WP2's battery-management port, not deferred. New driver logic: `svc_battery.c` (or a new charge-control module) needs to actively drive this pin — today it only reads `CHG_SENSE`/`CHARGE_SENSE`. |
| **In scope for WP2 port:** standby state sense | *(no equivalent — not implemented in wp2 branch)* | PD5, GPIO Input (`STANDBY_SENSE`) | Confirmed by user as needed for WP2's battery-management port. TP4056 STANDBY output — distinct signal from `CHARGE_SENSE`/CHRG. Needs a new read path in `svc_battery.c`'s state classification (NORMAL/LOW/CRITICAL/CHARGING/FULL) — likely how "FULL" gets detected now. |
| EEPROM I2C (24LC256) | PB6 (`I2C1_SCL`, AF6) / PB7 (`I2C1_SDA`, AF6) | PB6 (`I2C1_SCL`) / PB7 (`I2C1_SDA`) | **Exact match** — same pins, same peripheral instance (I2C1). Cleanest migration in this whole survey. |
| **Deferred:** BME280 environmental sensor | *(no equivalent — not referenced anywhere in WP2–WP5)* | Shares I2C1 (PB6/PB7) with EEPROM, per CSV comment | Confirmed by user as new-to-this-revision hardware to be addressed later, not part of the WP2 pin migration itself. Bus/pins are already a clean match; only the device (address, register map, driver) is new whenever it's picked up. |
| VREFINT / VREFBUF reference | Internal VREFBUF, scale 2.048V (CubeMX config, no pin) | VREF+ (physical pin 7) tied directly to `3V3_STANDBY` per CSV | If the new board doesn't route VREFBUF the same way, `LM35_SCALE`/`VBAT_SCALE_*` conversion math (assumes a known Vref) needs re-derivation against actual VDDA. Flag for hardware confirmation. |

---

## WP3 — Encoders, Buzzer, UI State Machine
*(feature commit `f2646e4`, CubeMX regen `880c218` excluded)*

| Signal / Function | Old pin/peripheral (branch) | New pin/peripheral (CSV) | Notes |
|---|---|---|---|
| Encoder 1, signal A | PA0, EXTI0 | PC4, EXTI4 (`ENC_1A`) | Pin + EXTI line change. |
| Encoder 1, signal B | PA1, EXTI1 | PB0, EXTI0 (`ENC_1B`) | Pin + EXTI line change. PB0 is freed up on the new board since battery sense moved off it (originally to PA15, now PB11 after the item-15 hardware rework — either way, not PB0). |
| Encoder 1, switch | PA2, EXTI2 | PA0, `SYS_WKUP1` (`ENC_1SW`) | Pin moves, gains Standby wake-up (WKUP1). CLAUDE.md §8.4 already lists WKUP1/PA0 as a wake source; no WP branch currently implements Standby, so this is newly-usable hardware capability, not a functional regression. |
| Encoder 2, signal A | PA3, EXTI3 | PB1, EXTI1 (`ENC_2A`) | Pin + EXTI line change. PB1 is freed up since temp sense moved off it (originally to PC8, now PB12 after the item-15 hardware rework — either way, not PB1). |
| Encoder 2, signal B | PC4, EXTI4 | PB2, EXTI2 (`ENC_2B`) | Pin + EXTI line change. |
| Encoder 2, switch | PC5, EXTI5 | PC5, `SYS_WKUP5` (`ENC_2SW`) | **Same physical pin**, but gains Standby wake-up (WKUP5), unused by WP3 code today. |
| Buzzer PWM | PB3, `TIM1_CH2` (AF1, prescaler 63 → 1 MHz tick, dynamic ARR/CCR2) | PC9, `TIM3_CH4` (`BUZZER`) | **Peripheral instance change: TIM1 → TIM3** (CSV corrected from `TMR23_CH4` to `TMR3_CH4`). `hal_tim.c`'s prescaler math is derived from TIM1's specific APB clock and must be redone for TIM3. Also note TIM3 was the old display-VCOM timer (`TIM3_CH1`) — the new pinout no longer assigns VCOM a timer channel at all (see Open Questions #9), so TIM3 is free to host the buzzer on CH4, but if VCOM ends up needing a hardware timer toggle too, CH1 on the same TIM3 instance is a natural pairing worth considering during WP3/display design. **Also requires `5V_EN` asserted before use** (user-confirmed, see WP2 table and Open Questions #7) — `drv_buzzer.c`/`app_scheduler.c`'s buzzer task will need to coordinate with whatever module owns the 5V rail rather than firing unconditionally on encoder events like it does today. |
| Encoder EXTI dispatch mechanism | `HAL_GPIO_EXTI_Rising/Falling_Callback`, keyed by pin mask (port-agnostic) in `hal_gpio.c` | n/a | No code dependency on which EXTI line number a given encoder pin lands on — the pin/EXTI-line changes above are CubeMX-config + `pin_config.h` updates only, not an algorithm change. |

---

## WP4 — USB Custom HID
*(feature commits `d1423b5`, `48e8562`; CubeMX regen `ff054fb` excluded)*

| Signal / Function | Old pin/peripheral (branch) | New pin/peripheral (CSV) | Notes |
|---|---|---|---|
| USB D− / D+ | PA11 (`USB_DM`) / PA12 (`USB_DP`) — CubeMX standard AF, auto-assigned, no explicit `pin_config.h` entry | PA11 (`USB_D-`/`USB_DM`) / PA12 (`USB_D+`/`USB_DP`) | **Exact match.** Also resolves CLAUDE.md's Open Item 3 (an *older* Netlist.md hardware rev had non-standard USB pins on PA9/PC6) — this new revision uses the standard STM32 USB FS pins the WP4 code already assumed. |
| VBUS sense (used by USB stack) | Same `VBUS_SENSE` pin as WP2 (PA10) | Same `VBUS_SENSE` pin as WP2 (PA2) | `hal_usb.c` and `svc_battery.c` both read this one physical signal — see WP2 table. |
| USB VID/PID/report descriptor | `Config/config.h` constants only, no pin dependency | n/a | Not part of this pinout survey — flagged here only as a reminder that VID 0x04D8 is borrowed/informal per the WP4 commit message and should be revisited before shipping. |

---

## WP5 — BLE (RN4871)
*(feature commits `1662959`, `73aa050`; CubeMX regen `4bc4f16` and docs-only `81a9643` excluded)*

| Signal / Function | Old pin/peripheral (branch) | New pin/peripheral (CSV) | Notes |
|---|---|---|---|
| BLE UART, MCU→RN4871 | PD5, `USART2_TX` (AF0) | PB8, `USART6_TX` (`BLE_UART_MCU_TO_BLE`) | **Peripheral instance change: USART2 → USART6.** `hal_uart.c` hardcodes `huart2`/`hdma_usart2_rx` — needs porting to the USART6 handle and confirming DMA channel availability for USART6 in CubeMX. |
| BLE UART, RN4871→MCU | PD6, `USART2_RX` (AF0) | PB9, `USART6_RX` (`BLE_UART_BLE_TO_MCU`) | Same instance change as above. |
| BLE reset | PD4, GPIO Output, active-LOW | PB5, GPIO Output, active-LOW (`!BLE_RESET!`) | Pin moves, polarity unchanged — straightforward `#define` update. |
| **New (to code):** BLE mode/status lines | Not referenced anywhere in `drv_rn4871.c` — the driver only ever touches RESET + UART TX/RX | PB15 (`BLE_P1_3`), PA8 (`BLE_P1_7`), PA9 (`BLE_P1_6`) — all `GPIO Input` | CLAUDE.md's older Netlist.md (§5.6) documents similar RN4871 GPIO lines (`BLE_P0_2`, `BLE_P1_6`, `BLE_P1_7`, `BLE_P2_0`, `BLE_P3_6`) for mode/status indication, so this is recurring *planned* functionality — but it was never wired into `drv_rn4871.c`'s state machine in any WP branch, so it still needs new driver code, not just pin defines. |
| WP5's own pin-conflict workaround | Commit `73aa050` relocated RN4871 UART off the USART2-default pins specifically to avoid clashing with WP3's encoder pins on PA2/PA3 | New pinout puts encoders on PA0–PA3/PB0–PB2 and BLE UART on PB8/PB9 (USART6) | The conflict that drove that relocation commit doesn't reoccur here — encoders and BLE UART no longer compete for the same pins. No action needed; noted for context on why WP5 carries an extra commit. |

---

## Open Questions / Conflicts

1. **RESOLVED — SCL3300 and PCAP04 are confirmed gone from this hardware revision.**
   Confirmed by user: these sensors are no longer part of the design. This does **not**
   block WP2–WP5, which are scoped to general UI and communication topics rather than the
   precision-sensor subsystem — no action needed until whatever future WP takes over the
   sensor front end.

2. **DEFERRED, not blocking — unidentified "ADC" (SPI1) / "DAC" (SPI3) chip pair.** The
   new CSV defines a 9-signal front end — `ADC_CS`/`SCK`/`MISO`/`MOSI` (SPI1),
   `ADC_SYNC_RESET` (GPIO out), `ADC_READY` (GPIO in), `ADC_CLOCK` (TIM2_CH3),
   `DAC_MOSI`/`FSYNC` (SPI3), `DAC_Clock` (TIM1_CH4) — that matches nothing in CLAUDE.md or
   any WP2–WP5 branch, and isn't the SCL3300/PCAP04 replacement per item 1. Since WP2–WP5
   don't touch this hardware, it doesn't need to be identified now — revisit when planning
   whichever WP owns the precision-sensor front end.

3. **RESOLVED — debug UART TX/RX corrected in the CSV.** Now reads
   `PD8;DEBUG_UART_MCU_TO_PC;USART3_TX` and `PD9;DEBUG_UART_PC_TO_MCU;USART3_RX`, consistent
   with the signal naming. Note this isn't a WP2–WP5 concern — debug UART is WP1.5
   (`db251c2`/`d3d77c0`), not in any of the branches this survey covers, and **not on
   `master`** either (**correction:** an earlier version of this doc said "master only" —
   those two commits actually sit on an unmerged side branch, `claude/lucid-thompson-923f35`;
   `master` itself has no debug UART yet). Worth flagging anyway since that branch's
   `pin_config.h`/Netlist.md has TX/RX the other way round (`DEBUG_UART_MCU_TO_PC`=PD9/TX,
   `DEBUG_UART_PC_TO_MCU`=PD8/RX) — so porting WP1.5 to this pinout swaps both the pins *and*
   which one is TX vs RX, not a same-role pin move.

4. **RESOLVED — buzzer timer is TIM3_CH4.** User confirmed the CSV had a typo:
   `TMR23_CH4` should read `TMR3_CH4` (PC9). The old buzzer code's prescaler/ARR math
   (`hal_tim.c`) still needs to be redone for TIM3's APB clock instead of TIM1's, and see
   the WP3 table note re: TIM3 also being the old VCOM timer.

5. **RESOLVED — HSE crystal is still 8 MHz.** User confirmed the "16MHz" comment in the
   original CSV was a typo; the corrected CSV reads "High-speed oscillator crystal 8MHz" on
   both PF0/PF1. No PLL/SYSCLK rework needed — CLAUDE.md §4's clock table stands as-is.

6. **CONFIRMED — 3V3 rail enable polarity is inverted, not just relocated.** User confirmed
   this is real, not a CSV typo. Old `LDO_EN` (PC0) is active-HIGH; new `!3V3_EN!` (PC6) is
   explicitly active-LOW ("Low=on, High=off"). `main.c`'s boot sequence asserts this pin
   *before* `HAL_Init()` via LL inline calls — still the highest-severity item in this
   survey, since a naive pin-rename without flipping the assert logic could leave the 3.3V
   rail permanently disabled or bypass intended power sequencing entirely.

7. **RESOLVED — 5V_EN (PC7) power sequencing was a pre-existing gap, not new scope.** User
   confirmed the ±5V rail requirement (display, buzzer, on-board temp sensor, external temp
   sensor, DAC/ADC front end) was always there — it was simply never implemented in the
   WP2–WP5 branches, which is being corrected as part of this migration rather than
   discovered fresh here. Consumer list, relative to this repo's branch structure:
   - **WP2** (this survey's scope): both temp sensors need it — added to the WP2 table above.
   - **WP3** (this survey's scope): the buzzer needs it — added to the WP3 table above.
   - **WP1 / display**: the Sharp LCD is already implemented on `master`, on hardware that
     predates this CSV and had no 5V rail at all. Bringing WP1's display code onto this new
     pinout (not part of this survey's WP2–WP5 scope) will need the same `5V_EN`
     precondition added.
   - **DAC/ADC front end** (Open Questions #2): also gated by `5V_EN`, but that subsystem's
     scope is itself still unidentified, so this is a placeholder dependency until it's
     picked up.

   Practical implication for the WP2 port: whichever module ends up owning `5V_EN`
   (naturally WP2's power module, alongside `3V3_EN`) needs a **reference-counted or shared
   enable**, not a simple on/off tied to one feature — display, buzzer, and two independent
   temp-sense call sites all need the rail up, and it should probably stay enabled as long
   as any one of them is active rather than being toggled per-caller.

8. **IN SCOPE FOR WP2 — STANDBY_SENSE (PD5) and CHARGE_EN (PD6).** User confirmed these
   are needed now, as part of WP2's battery-management port, not deferred. Added as new
   rows in the WP2 table above. `CHARGE_EN` is a new active-high output letting firmware
   actively enable/disable charging (old code only ever sensed charge state via
   `CHG_SENSE`/`CHARGE_SENSE`); `STANDBY_SENSE` is a distinct TP4056 output from
   `CHARGE_SENSE` and needs a new read path in `svc_battery.c`.

9. **Display CS/VCOM lose their old hardware timer/NSS association:**
   - `DISP_CS` (PD0) is labeled `SPI2_NSS`, but the display's CS is active-**HIGH** per
     CLAUDE.md/old code, while standard STM32 hardware NSS is active-low. Confirm hardware
     NSS actually supports active-high, or keep bit-banging CS as a plain GPIO output (as
     the old SPI1 bit-banged approach effectively did for Open Item 4).
   - `DISP_VCOM` (PD3) is listed as plain `GPIO Output` with **no timer channel**, whereas
     the old design hardware-PWM'd it via `TIM3_CH1`. Since VCOM must toggle ≥1 Hz
     continuously or the display is permanently damaged (CLAUDE.md's critical constraint),
     firmware will need a dedicated timer ISR that manually toggles this GPIO rather than
     relying on a hardware PWM pin. Not a blocker, but safety-critical enough to call out
     explicitly before implementation.
   - Good news: the display now cleanly maps to SPI2 (PD0/PD1/PD4, all one GPIO port) —
     this resolves CLAUDE.md's Open Item 4 (the old cross-peripheral AF conflict across
     PB0/PB10/PB11 that forced bit-banged SPI).

10. **SWD conflict (CLAUDE.md Open Item 1) appears resolved.** PA13/PA14 are dedicated
    `SYS_SWDIO`/`SYS_SWCLK` in the new CSV with no competing peripheral — consistent with
    item 1 above (the sensors that caused the old conflict aren't in this pinout at all).

11. **USB pin mapping (CLAUDE.md Open Item 3) is resolved** — new CSV uses standard
    PA11(D-)/PA12(D+), matching what WP4's code already assumed via CubeMX auto-assign.

12. **BLE UART instance changed: USART2 → USART6.** `hal_uart.c` is written directly
    against `huart2`/`hdma_usart2_rx`; porting requires the USART6 equivalents and
    confirming USART6 has usable DMA channels on this part.

13. **WP numbering mismatch between git branches and CLAUDE.md §10** (see top of this
    document). Worth reconciling before further planning so "WP2" means the same thing in
    conversation, commits, and CLAUDE.md.

14. **CSV row 66 is blank** (trailing empty line after PC10) — not a conflict, just noting
    the file has a trailing blank record in case it trips up any CSV tooling used later.

15. **RESOLVED via hardware rework — `BATTERY_SENSE`/`TEMP_SENSE`/`TEMP_SENSE_EXT` weren't
    real ADC1 channels.** PA15, PC8, and PA10 (their original CSV assignments) all allowed
    CubeMX's "Analog" GPIO mode to be selected, which misled the pin selection — that mode
    just electrically isolates a pin, it doesn't confirm an actual ADC channel is wired to
    it. Caught after the prototype board was already fabricated; fixed via manual bodge
    rework, moving the three sense signals to PB11/PB12/PA3 (real ADC1 pins) and displacing
    the plain-GPIO signals that were there (`LED_STATUS`, `LED_PWR`, `CHARGE_SENSE`) to free
    "Not Connected" pins. See the WP2 table above and
    `docs/cubemx_configuration_checklist.md` §0 for the full 6-pin chain. **Worth
    remembering as a general lesson for any future pin selection on this project:** always
    confirm CubeMX shows a real `ADCx_INy` assignment for a pin, not just that "Analog" mode
    is offered.
