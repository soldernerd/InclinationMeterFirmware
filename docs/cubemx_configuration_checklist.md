# CubeMX Configuration Checklist — WP2–WP5 on New Hardware Pinout

**Purpose:** a single-pass, manual GUI checklist for configuring `InclinationMeterFirmware.ioc`
from scratch against `STM32G0B1RET6_Pinout.csv`, covering every pin/peripheral needed by
WP2–WP5. Document only — nothing here edits the `.ioc`, generated code, or runs CubeMX.

**Sources of truth used:** `docs/pinout_migration_wp2-5.md` (resolved facts), the current
`STM32G0B1RET6_Pinout.csv`, and driver/HAL source + `.ioc` parameter values read from
`master`, `wp2`–`wp5`, and the unmerged branch `claude/lucid-thompson-923f35` (WP1.5 debug
UART reference only — **not** part of WP2–WP5 scope, included because the CSV defines its
pins and it's needed for a genuinely from-scratch `.ioc`).

**Two things flagged in the previous draft, now resolved by user confirmation:**
- **HSE = 8 MHz, confirmed.** The "16 MHz" in the original task brief was wrong.
- **PB10/PC10 pin 64 mislabeling, confirmed and fixed.** An earlier CSV draft had pin 64
  labeled `PB10`; it's `PC10` (`DAC_SCK`). `ADC_CLOCK` is PB10 (pin 30, `TIM2_CH3`) and
  `DAC_SCK` is PC10 (pin 64, `SPI3_SCK`) — two distinct pins, no duplicate, no further
  action needed.

**Also confirmed by user:** encoder EXTI pins (and the analogous polled `ENC_1SW`) need
**no internal pull** — they're driven by CMOS-level signals from the encoder interface
hardware (RC filter + 74HC14 Schmitt trigger per CLAUDE.md §5.11), not open-collector
switches, so an internal pull would be redundant. This replaces the "ASSUMED pull-up" calls
in the previous draft for all six encoder signal pins.

---

## 0. Hardware Rework — Analog Pin Reassignment (Prototype Board Fix)

**Root cause:** three pins chosen for internal-ADC1 use — `BATTERY_SENSE` (was PA15),
`TEMP_SENSE` (was PC8), `TEMP_SENSE_EXT` (was PA10) — turned out not to be real ADC1-capable
pins on this package, despite CubeMX allowing "Analog" mode to be selected for them.
**CubeMX's generic "Analog" GPIO mode just electrically isolates a pin (disables the digital
input/output buffers) — it does not confirm that pin is wired to an actual ADC channel.**
Every GPIO pin can be set to Analog mode; only a specific subset are physically muxed to an
ADC peripheral. This was caught after the prototype board was already fabricated, so it's
being fixed as a manual bodge/rework on the existing board, with the pinout CSV updated to
match and stand as the corrected source of truth going forward.

**The reassignment (a 6-pin chain, not 3 independent moves)** — the three sense signals move
to pins that *are* real ADC1 channels; whatever plain-GPIO signal previously sat on each
destination pin gets displaced to a free "Not Connected" pin, since plain GPIO has no ADC
constraint to satisfy:

| Signal | Old pin | New pin | Why |
|---|---|---|---|
| `BATTERY_SENSE` | PA15 | **PB11** | PA15 not ADC1-capable; PB11 is. |
| `TEMP_SENSE` | PC8 | **PB12** | PC8 not ADC1-capable; PB12 is. |
| `TEMP_SENSE_EXT` | PA10 | **PA3** | PA10 not ADC1-capable; PA3 is. |
| `LED_STATUS` (displaced) | PB11 | **PB13** | Freed PB11 for `BATTERY_SENSE`; LEDs have no ADC constraint, any free pin works. |
| `LED_PWR` (displaced) | PB12 | **PB14** | Freed PB12 for `TEMP_SENSE`. |
| `CHARGE_SENSE` (displaced) | PA3 | **PC2** | Freed PA3 for `TEMP_SENSE_EXT`. |

PA15, PC8, and PA10 are now vacated — treat them as "Not Connected" (see §1).

**Carried-forward risk, flagged explicitly: this checklist has not independently verified
that PB11/PB12/PA3 actually are ADC1-capable pins either** — that claim comes from the
user's hardware-level fix, not from something checkable in the CSV or driver code. Given
this is exactly the class of mistake that caused the rework in the first place, **the single
most important item in this update is in §5: confirm in CubeMX itself that each of these
three pins shows a real `ADC1_INx` assignment**, not just that "Analog" mode is selectable,
before trusting the pin table below a second time.

---

## 1. Pin-by-Pin Configuration Table

Ordered by physical `PinNumber` (1–64), matching the CSV exactly. "GPIO settings" gives
output initial-level/pull/speed, or input pull, where applicable. Anything not explicitly
stated by the CSV or existing driver code is marked **ASSUMED — verify**.

| # | Pin | User Label | Mode/Signal | GPIO Settings | Notes |
|---|-----|-----------|-------------|----------------|-------|
| 1 | PC11 | `DAC_Clock` | TIM1_CH4 (Output Compare or PWM) | ASSUMED — frequency/mode unknown until DAC chip identified | **Deferred** (Open Questions #2 in migration doc). Enable the pin/AF now so the `.ioc` is complete; leave TIM1 CH4 parameters as CubeMX defaults. |
| 2 | PC12 | `DAC_MOSI` | SPI3_MOSI (AF) | N/A (AF pin) | Deferred, see SPI3 section below. |
| 3 | PC13 | `DAC_FSYNC` | **`SPI3_NSS` (AF)** — hardware NSS, per user's chip-select policy | N/A (AF pin) if hardware NSS; ASSUMED initial HIGH (idle-deselected) if it falls back to GPIO_Output | Deferred. **Default hardware NSS** now (§2 SPI3) — ASSUMED fine for a conventional active-low CS, fall back to software GPIO only if testing/the DAC datasheet says otherwise once the chip is known. |
| 4 | PC14 | *(none)* | RCC_OSC32_IN | N/A — reserved by RCC | LSE 32.768 kHz crystal input. See §3 — this resolves CLAUDE.md Open Item 2 differently than originally planned (LSI fallback); **ASSUMED crystal Y1 is populated on this revision — verify with hardware designer**, since CLAUDE.md's original recommendation was LSI specifically because Y1 was expected unpopulated. |
| 5 | PC15 | *(none)* | RCC_OSC32_OUT | N/A — reserved by RCC | Paired with PC14, see above. |
| 6 | VBAT | `3V3_STANDBY` | VBAT | N/A — power pin | No CubeMX pin action. |
| 7 | VREF+ | `3V3_STANDBY` | VREF+ | N/A — power pin | Tied directly to the always-on 3V3 rail per CSV — see ADC1/VREFBUF note in §2. |
| 8 | VDD | `3V3_STANDBY` | VDD | N/A — power pin | No CubeMX pin action. |
| 9 | VSS | `GND` | VSS | N/A — ground | No CubeMX pin action. |
| 10 | PF0 | *(none)* | RCC_OSC_IN | N/A — reserved by RCC | HSE 8 MHz crystal input — confirmed by user. |
| 11 | PF1 | *(none)* | RCC_OSC_OUT | N/A — reserved by RCC | HSE 8 MHz crystal output. |
| 12 | !NRST! | `!RST!` | Reset state | N/A — leave as Reset/RST | Do not repurpose. |
| 13 | PC0 | Not connected | Reset state | **ASSUMED — set to Analog** to minimize floating-pin current on a battery-powered device | CSV lists as "Reset state"/NC; CubeMX's own Reset state already draws minimal current, but explicit Analog mode is the conventional recommendation for genuinely unused pins. Verify this doesn't collide with a future use before generating code. |
| 14 | PC1 | Not connected | Reset state | Same as PC0 — ASSUMED Analog | |
| 15 | PC2 | `CHARGE_SENSE` | GPIO_Input | ASSUMED pull-up (TP4056 CHRG is open-drain active-low, per old `CHG_SENSE` code) | **Moved here from PA3** (see §0 rework note). Plain input, polled by `svc_battery.c`, not EXTI. |
| 16 | PC3 | `ADC_SYNC_RESET` | GPIO_Output | ASSUMED push-pull, low speed, initial level unknown (active polarity not stated) | Deferred — front-end reset/sync line for the unidentified ADC chip. Initial level is a guess until the chip's datasheet is available. |
| 17 | PA0 | `ENC_1SW` | **`PWR_WKUP1`** (Signal, per user decision — mutually exclusive with GPIO_Input/EXTI in CubeMX's Pinout view) | **No pull** (CMOS-driven encoder signal, confirmed by user — not an open-collector switch). **Verify GPIO Input mode/pull settings remain editable and the pin isn't left in Analog mode** — see §4. | **Not EXTI0 either way** — PB0/`ENC_1B` already owns EXTI line 0, so this pin was always going to be polled, not interrupt-driven; choosing `WKUP1` costs nothing here. WKUP1 itself unused until WP6 implements Standby, but configured now per user preference. |
| 18 | PA1 | `ADC_READY` | GPIO_Input | ASSUMED no pull (typically a driven ready/busy output from the external ADC chip) | Deferred — plain input per CSV (not `GPIO_EXTI1`), consistent with polling rather than interrupt-driving it once that driver exists. |
| 19 | PA2 | `VBUS_SENSE` | **`PWR_WKUP4`** (Signal, per user decision) | ASSUMED no pull (actively driven by external divider). **Same Analog-mode verification as PA0** — see §4. | Old code (`hal_usb.c`/`svc_battery.c`) polls this pin and never used EXTI, so choosing `WKUP4` costs nothing functionally. WKUP4 itself unused until WP6, configured now per user preference. |
| 20 | PA3 | `TEMP_SENSE_EXT` | Analog (ADC1 input) | N/A — Analog mode disables pull automatically | **Moved here from PA10** (see §0 rework note) — PA3 is a real ADC1-capable pin, unlike PA10. Needs `5V_EN` (see migration doc item 7). |
| 21 | PA4 | `ADC_CS` | **`SPI1_NSS` (AF)** — hardware NSS, per user's chip-select policy | N/A (AF pin) if hardware NSS; ASSUMED initial HIGH (idle-deselected) if it falls back to GPIO_Output | Deferred. **Default hardware NSS** now (§2 SPI1) — ASSUMED fine for a conventional active-low CS, fall back to software GPIO only if testing/the ADC datasheet says otherwise once the chip is known. |
| 22 | PA5 | `ADC_SCK` | SPI1_SCK (AF) | N/A (AF pin) | Deferred, see SPI1 section. |
| 23 | PA6 | `ADC_MISO` | SPI1_MISO (AF) | N/A (AF pin) | Deferred. |
| 24 | PA7 | `ADC_MOSI` | SPI1_MOSI (AF) | N/A (AF pin) | Deferred. |
| 25 | PC4 | `ENC_1A` | GPIO_EXTI4 | **No pull**, both-edge trigger — confirmed by user (CMOS-driven, not open-collector) | Interrupt-driven, matches `hal_gpio.c`'s Rising/Falling dispatch. Note the old `.ioc` also had no pull configured here despite the WP3 banner text saying to set one — turns out that was correct all along, not an oversight. |
| 26 | PC5 | `ENC_2SW` | **`PWR_WKUP5`** (Signal, per user decision — **supersedes the earlier `GPIO_EXTI5` plan**) | **No pull** (CMOS-driven, confirmed by user). **Same Analog-mode verification as PA0** — see §4. | **Real trade-off, not a free choice** — see §4. This pin had no EXTI line conflict and was going to stay interrupt-driven (the one switch that kept old WP3 behavior); choosing `WKUP5` gives it up. `ENC_2SW` is now polled, same as `ENC_1SW` — `drv_encoder.c` needs both switches handled by polling, not EXTI callbacks. |
| 27 | PB0 | `ENC_1B` | GPIO_EXTI0 | **No pull**, both-edge trigger — confirmed by user | Mandatory per CSV — this is the pin that occupies EXTI line 0 (see PA0 note above). |
| 28 | PB1 | `ENC_2A` | GPIO_EXTI1 | **No pull**, both-edge trigger — confirmed by user | |
| 29 | PB2 | `ENC_2B` | GPIO_EXTI2 | **No pull**, both-edge trigger — confirmed by user | |
| 30 | PB10 | `ADC_CLOCK` | TIM2_CH3 (Output Compare or PWM) | ASSUMED — frequency unknown until ADC chip identified | Deferred. Distinct pin from `DAC_SCK`/PC10 — see correction at top of doc. |
| 31 | PB11 | `BATTERY_SENSE` | Analog (ADC1 input) | N/A — Analog mode | **Moved here from PA15** (see §0 rework note) — PB11 is a real ADC1-capable pin, unlike PA15. Divider ratio confirmed unchanged (100k/68k). |
| 32 | PB12 | `TEMP_SENSE` | Analog (ADC1 input) | N/A — Analog mode | **Moved here from PC8** — PB12 is a real ADC1-capable pin, unlike PC8. On-board temp sensor, needs `5V_EN`. |
| 33 | PB13 | `LED_STATUS` | GPIO_Output | Active-HIGH (CSV explicit), push-pull, **ASSUMED initial LOW** (off until firmware decides), low speed | **Moved here from PB11** to free that pin for `BATTERY_SENSE` (see §0 rework note). LEDs don't need ADC capability, so any free pin works — this one just happened to need to move to make room. |
| 34 | PB14 | `LED_PWR` | GPIO_Output | Active-HIGH (CSV explicit), push-pull, **ASSUMED initial LOW**, low speed | **Moved here from PB12** to free that pin for `TEMP_SENSE`. Whether this should default ON at boot is a firmware/UX decision, not a CubeMX one. |
| 35 | PB15 | `BLE_P1_3` | GPIO_Input | ASSUMED no pull | New-to-code (Open Questions in migration doc) — `drv_rn4871.c` doesn't consume this today. Configure as input now; pull/usage TBD against the RN4871 datasheet when that driver work happens. |
| 36 | PA8 | `BLE_P1_7` | GPIO_Input | ASSUMED no pull | Same as PB15 — not consumed by current driver. |
| 37 | PA9 | `BLE_P1_6` | GPIO_Input | ASSUMED no pull | Same as PB15 — not consumed by current driver. |
| 38 | PC6 | `!3V3_EN!` | GPIO_Output | Active-**LOW** (confirmed), push-pull, **ASSUMED initial HIGH** (rail OFF — safe default), low speed | Highest-severity item in the whole migration — see migration doc item 6. Recommend the CubeMX reset-state default leaves the rail *off* (HIGH) since `main.c`'s early boot code is what actively drives it low, not CubeMX's GPIO init. |
| 39 | PC7 | `5V_EN` | GPIO_Output | Active-HIGH (CSV explicit), push-pull, **ASSUMED initial LOW** (rail OFF), low speed | Needs a shared/reference-counted enable across WP1 (display), WP2 (both temp sensors), WP3 (buzzer) — see migration doc item 7. Not a CubeMX-level concern beyond the pin itself. |
| 40 | PD8 | `DEBUG_UART_MCU_TO_PC` | USART3_TX (AF) | N/A (AF pin) | Not WP2–WP5 scope (WP1.5) — included for pinout completeness, see §2 USART3. |
| 41 | PD9 | `DEBUG_UART_PC_TO_MCU` | USART3_RX (AF) | ASSUMED no pull (STLINK VCP actively drives the line) | Same as above. |
| 42 | PA10 | Not connected | Reset state | **ASSUMED — set to Analog** to minimize floating-pin current | **Vacated — `TEMP_SENSE_EXT` moved to PA3** (see §0 rework note). PA10 turned out not to be a real ADC1-capable pin despite CubeMX allowing "Analog" mode here — that's the root cause of this whole rework. |
| 43 | PA11 | `USB_D-` | USB_DM (AF) | N/A (AF pin, managed by USB peripheral) | |
| 44 | PA12 | `USB_D+` | USB_DP (AF) | N/A (AF pin) | |
| 45 | PA13 | `SWDIO` | SYS_SWDIO | **Leave CubeMX default "Serial Wire" debug mode — do not repurpose** | Dedicated, no competing peripheral (resolves CLAUDE.md Open Item 1). |
| 46 | PA14 | `SWCLK` | SYS_SWCLK | Leave CubeMX default "Serial Wire" | Same as above. |
| 47 | PA15 | Not connected | Reset state | **ASSUMED — set to Analog** to minimize floating-pin current | **Vacated — `BATTERY_SENSE` moved to PB11** (see §0 rework note). Same root cause as PA10/PC8: not a real ADC1-capable pin. |
| 48 | PC8 | Not connected | Reset state | **ASSUMED — set to Analog** to minimize floating-pin current | **Vacated — `TEMP_SENSE` moved to PB12** (see §0 rework note). Same root cause as PA10/PA15. |
| 49 | PC9 | `BUZZER` | TIM3_CH4 (PWM) | N/A (AF pin, timer-driven) | See §2 TIM3. Needs `5V_EN` asserted before use. |
| 50 | PD0 | `DISP_CS` | **`GPIO_Output`** (software CS — verified exception to the hardware-NSS-by-default policy, see §2 SPI2) | Active-HIGH, push-pull, initial HIGH-or-LOW per whichever level = deselected | Display CS is active-HIGH (CLAUDE.md/old code); this SPI IP's hardware NSS output is hard-wired active-low with no polarity control (confirmed by inspecting `stm32g0xx_hal_spi.h` in this repo — no `SSIOP`-equivalent bit exists here). Not a "try it and see" — genuinely can't be done in hardware, so software GPIO CS is used from the start, mirroring `hal_spi_cs_assert/deassert` on the old SPI1 display code. |
| 51 | PD1 | `DISP_SCK` | SPI2_SCK (AF) | N/A (AF pin) | |
| 52 | PD2 | `DISP_ON` | GPIO_Output | Push-pull, **initial LOW** (off until `drv_sharp_lcd_init()` powers it on — matches existing driver code exactly) | |
| 53 | PD3 | `DISP_VCOM` | GPIO_Output, toggled by a dedicated timer ISR | Push-pull, initial LOW, **ASSUMED — must start toggling ≥1 Hz as soon as the display is powered** | No hardware timer channel assigned by CSV (unlike the old `TIM3_CH1` PWM approach) — see §2 TIM6 recommendation and §4. Safety-critical per CLAUDE.md; do not treat this row as "just a GPIO." |
| 54 | PD4 | `DISP_MOSI` | SPI2_MOSI (AF) | N/A (AF pin) | |
| 55 | PD5 | `STANDBY_SENSE` | GPIO_Input | ASSUMED pull-up (TP4056 STANDBY is typically open-drain active-low, mirroring `CHARGE_SENSE`) | New-to-code, needed for WP2 (migration doc item 8). ASSUMED — verify against TP4056 datasheet. |
| 56 | PD6 | `CHARGE_EN` | GPIO_Output | Active-HIGH (CSV explicit), push-pull, **ASSUMED initial HIGH** (charging enabled — preserves old hardware-autonomous charging behavior until firmware actively decides to gate it) | New-to-code, needed for WP2. Initial level is a firmware-policy call, not a hardware constraint — flagging the reasoning rather than asserting it. |
| 57 | PB3 | `SWO` | Reset state / unused | **ASSUMED Analog** (CSV: "Signal to debug header. Unused"; CLAUDE.md: "not functional on Cortex-M0+") | Free — no longer the buzzer pin (that moved to PC9/TIM3). |
| 58 | PB4 | `BLE_P1_2` | GPIO_Input | ASSUMED no pull | Not consumed by current `drv_rn4871.c` — same treatment as PB15/PA8/PA9. |
| 59 | PB5 | `!BLE_RESET!` | GPIO_Output | Active-LOW, push-pull, **no pull, low speed, initial HIGH** (not held in reset) | Exact spec carried over from the WP5 CubeMX banner (`main.c` on `wp5`) — only the pin number changed (PD4 → PB5). |
| 60 | PB6 | `EEPROM_SCL` | I2C1_SCL (AF) | Open-drain (I2C default), **ASSUMED no internal pull** — relies on external bus pull-up resistors | Exact pin match with old code — see §2 I2C1. |
| 61 | PB7 | `EEPROM_SDA` | I2C1_SDA (AF) | Same as PB6 | |
| 62 | PB8 | `BLE_UART_MCU_TO_BLE` | USART6_TX (AF) | N/A (AF pin) | Peripheral instance change from old USART2 — see §2 USART6. |
| 63 | PB9 | `BLE_UART_BLE_TO_MCU` | USART6_RX (AF) | N/A (AF pin) | Same instance change. |
| 64 | PC10 | `DAC_SCK` | SPI3_SCK (AF) | N/A (AF pin) | Deferred. Distinct from PB10/`ADC_CLOCK` — see correction at top of doc. |

---

## 2. Peripheral-by-Peripheral Configuration

**Chip-select policy for all three SPI peripherals (user directive):** default to hardware
NSS/SSN (`SPI_NSS_HARD_OUTPUT`) wherever possible; only fall back to software-controlled
GPIO if testing shows hardware NSS won't work for a given peripheral. Applied below — one
of the three (SPI2/display) already has a verified reason it can't use hardware NSS, the
other two default to hardware NSS pending verification once their target chips are known.

### SPI1 — External Precision ADC Front End *(deferred — pins only)*
- **Mode:** Full-Duplex Master. **ASSUMED — clock polarity/phase and baud rate are unknown**
  until the ADC chip (Open Questions #2) is identified. Enable the peripheral and assign its
  four pins (PA4–PA7) now so the `.ioc` is complete; leave protocol parameters at CubeMX
  defaults (Mode 0, moderate prescaler) as placeholders.
- **NSS:** **Default to hardware NSS** (`SPI1_NSS` on PA4, `SPI_NSS_HARD_OUTPUT`) per the
  chip-select policy above. Hardware NSS output on this SPI IP is fixed active-low (see the
  SPI2/display note below for why — same peripheral, same constraint) — this is fine as
  long as the eventual ADC chip's `ADC_CS` is a conventional active-low chip-select, which
  is the common case. **ASSUMED — verify once the chip is identified**, and fall back to
  software GPIO only if the chip needs unusual CS timing (e.g. an active-high CS like the
  display, or CS pulses that don't align with standard hardware NSS behavior).
- No DMA/NVIC configuration recommended yet — nothing consumes it.

### SPI2 — Display (Sharp LS027B7DH01)
- **Mode:** Full-Duplex Master (display is write-only, but this matches the old SPI1
  config which also left Direction at 2-lines even though MISO is unused — **ASSUMED**,
  could alternatively set Transmit-Only if you want to reclaim the pin, but display has no
  MISO pin defined in the CSV anyway so this is moot).
- **Data size:** 8-bit. **Clock polarity/phase:** Mode 0 (CPOL Low, CPHA 1 Edge) — CubeMX
  default, matches old SPI1 config which never overrode it.
- **Baud rate:** old SPI1 used prescaler 64 → ~1000 kbit/s from a 64 MHz APB2 clock.
  **ASSUMED SPI2's APB1 clock is also 64 MHz** on this simple single-domain clock tree (no
  APB prescaler) — verify in the Clock Configuration tab, then pick the SPI2 prescaler that
  lands closest to ~1 MHz.
- **NSS: software GPIO, and this one's verified rather than assumed — not a "try it and see."**
  Checked directly against this project's own `Drivers/STM32G0xx_HAL_Driver/Inc/stm32g0xx_hal_spi.h`:
  this SPI IP only exposes `SPI_NSS_SOFT` / `SPI_NSS_HARD_INPUT` / `SPI_NSS_HARD_OUTPUT` (via
  `SPI_CR2_SSOE`) — there's no NSS-polarity bit anywhere in this header (the newer STM32 SPI
  IP on H7/G4/L5 has an `SSIOP` polarity bit; this G0 part uses the older CR1/CR2-style IP,
  which doesn't). Hardware NSS output is hard-wired active-low with no way to invert it in
  the peripheral. The display's CS is active-HIGH (CLAUDE.md, old code) — a hard, verified
  incompatibility, not something that needs testing to discover. Keep software GPIO CS here
  (see pin table row 50, PD0), mirroring the existing `hal_spi_cs_assert/deassert` pattern.
- **DMA:** SPI2_TX, Normal mode, byte/byte, low priority — matches old SPI1_TX DMA config
  (`drv_sharp_lcd.c` uses `HAL_SPI_Transmit_DMA`). No RX DMA needed (write-only usage).
- **NVIC:** SPI2 global interrupt not required (DMA-driven); DMA channel transfer-complete
  interrupt is required (drives `on_dma_complete()` → CS deassert).

### SPI3 — External DAC *(deferred — pins only)*
- Same treatment as SPI1: enable the peripheral and its three pins (PC10, PC12, PC13), leave
  protocol parameters at CubeMX defaults, **ASSUMED — revisit entirely once the DAC chip is
  identified.**
- **NSS:** **Default to hardware NSS** (`SPI3_NSS`/`DAC_FSYNC` on PC13, `SPI_NSS_HARD_OUTPUT`)
  per the chip-select policy above, same reasoning and same caveat as SPI1 — fine for a
  conventional active-low CS, fall back to software GPIO if the DAC chip's CS requirements
  turn out to need something the hardware can't do (same active-low-only constraint applies
  here as it does on SPI2).

### I2C1 — EEPROM (BL24C256A) + BME280
- **Mode:** I2C Standard Mode, 100 kHz. Old `.ioc` computed `I2C1.Timing = 0x10B17DB5` for
  this speed at a 64 MHz I2C clock — **ASSUMED this timing value is still correct**, since
  the I2C1 pins/instance are an exact match to the old config and the clock tree is
  unchanged (verify in CubeMX's Timing calculator rather than hand-typing the hex value, in
  case the tool recomputes something different for this project's exact settings).
- **BME280 consideration (deferred by design):** BME280 can typically run Fast Mode
  (400 kHz), which would benefit sampling rate but isn't required. No pin/peripheral-level
  action needed either way — this is a software/timing decision to make later, not
  something that changes this checklist. Confirmed resolved for CubeMX purposes: I2C1 needs
  no BME280-specific configuration, since the bus/pins are already shared by address, not by
  pin config.
- **DMA:** I2C1_RX and I2C1_TX, both Normal mode, byte/byte, low priority — matches old
  `.ioc`.
- **NVIC:** I2C1 event interrupt + I2C1 error interrupt enabled — matches old `.ioc`/banner.

### ADC1 — Internal Battery/Temperature Sensing
*(Not the external "ADC" SPI1 front end above — this is the STM32's own internal ADC
peripheral, renamed here for clarity since the CSV's "ADC" label collides with it.)*
- **Mode:** Independent, one-shot triggered scan (not continuous) — matches `hal_adc.c`'s
  `HAL_ADC_Start_DMA()` on-demand pattern, not free-running.
- **Channels (scan order), 4 ranks now instead of 3:**
  1. VREFINT
  2. `BATTERY_SENSE` (**PB11**, moved from PA15 — see §0)
  3. `TEMP_SENSE` (**PB12**, moved from PC8 — see §0)
  4. `TEMP_SENSE_EXT` (**PA3**, moved from PA10 — see §0)
  **ASSUMED — do not hardcode old `IN8`/`IN9` channel numbers.** Let CubeMX auto-assign the
  correct `ADC1_INx` channel for each pin once its mode is set to Analog/ADC1 in the Pinout
  view. **Critical this time, not just a formality:** actually confirm CubeMX shows a real
  `ADC1_INx` channel number for all three pins — this exact "Analog mode selectable but no
  real ADC channel behind it" gap is what caused the §0 rework, so don't just trust the mode
  dropdown a second time.
- **Sampling time:** 160.5 cycles, common to both groups — matches old `.ioc`.
- **Oversampling:** Enabled, ratio 16×, right-shift 4 — matches old `.ioc`. `drv_lm35`'s
  and battery-sense conversion math assume this exact oversampling config; don't change it
  without recomputing `LM35_SCALE`/`VBAT_SCALE_*`.
- **VREFBUF:** old config used internal VREFBUF at 2.048 V scale. **Flag: the new CSV ties
  VREF+ directly to `3V3_STANDBY`** (see migration doc) — verify whether VREFBUF is still
  usable/intended on this board, or whether the ADC reference is now just VDDA. This
  changes the conversion math either way.
- **DMA:** ADC1, Normal mode, half-word/half-word alignment, low priority — matches old
  `.ioc`.
- **NVIC:** ADC1 global interrupt + DMA channel interrupt enabled — matches old banner.

### TIM1 — DAC Clock *(deferred)*
- PC11/`DAC_Clock`, Output Compare or PWM CH4. **ASSUMED — frequency/duty unknown** until
  the DAC chip is identified. Enable the channel now with placeholder ARR/CCR values;
  revisit fully later.

### TIM2 — ADC Clock *(deferred)*
- PB10/`ADC_CLOCK`, Output Compare or PWM CH3. Same treatment as TIM1 above — placeholder
  only.

### TIM3 — Buzzer PWM
- **Mode:** PWM Generation, Channel 4 (PC9).
- **Prescaler/ARR:** old buzzer code (TIM1 in the old scheme) used prescaler 63 to get a
  1 MHz timer tick from a 64 MHz APB clock, then computed `ARR = (1 MHz / freq_hz) − 1`
  dynamically at runtime for tones in the 500–4000 Hz range (`hal_tim.c`). **ASSUMED TIM3's
  APB clock is also 64 MHz** (same single-domain clock tree as SPI2 — verify), in which case
  the same prescaler=63 approach carries over unchanged; only the timer instance name in
  `hal_tim.c` needs updating (`htim1`→`htim3`, `TIM_CHANNEL_2`→`TIM_CHANNEL_4`).
- **Auto-reload preload:** Enable (matches old config).
- Set placeholder `Period`/`Pulse` values in CubeMX (e.g. 999/500, as the old `.ioc` did) —
  these get overwritten at runtime by `hal_tim`'s dynamic ARR/CCR logic.
- **NVIC:** none needed — buzzer start/stop is direct register writes, no interrupt used by
  old code.

### TIM6 — Display VCOM Toggle *(new — confirmed by user, not in the CSV)*
- **Why this is needed:** the new pinout gives `DISP_VCOM` (PD3) no timer channel at all —
  it's plain `GPIO_Output` (see pin table row 53). The old design hardware-PWM'd VCOM via
  `TIM3_CH1`, but TIM3 is now committed to the buzzer (CH4) with a completely different,
  dynamically-changing ARR (250–2000-ish at 1 MHz tick) that's incompatible with VCOM's
  fixed ~30 Hz requirement sharing the same counter. **A separate timer, used purely for a
  periodic update-interrupt that manually toggles PD3 in the ISR, is the cleanest fix.**
- **TIM6, confirmed** (a basic timer with no GPIO/channel of its own, ideal for a pure
  periodic-interrupt use case, and otherwise completely unused in this pinout).
- **Target rate:** old `TIM3` VCOM config was Prescaler=63999, Period=33 → ~29.4 Hz from a
  64 MHz clock. **ASSUMED the same values work for TIM6** if TIM6's clock is also 64 MHz —
  verify and adjust to land at ≥1 Hz (CLAUDE.md's hard constraint) with comfortable margin.
- **NVIC:** TIM6 global interrupt **must** be enabled — this is the only mechanism that
  drives the toggle. This is safety-critical (CLAUDE.md: VCOM must never stop toggling while
  the display is powered) — don't treat it as optional.

### USART3 — Debug UART *(WP1.5, not WP2–WP5 scope — included for pinout completeness)*
- **Mode:** Asynchronous, 115200 baud, 8 data bits, no parity, 1 stop bit — matches the
  reference implementation on `claude/lucid-thompson-923f35`.
- **DMA:** USART3_TX only, Normal mode, byte/byte — the reference implementation is a
  4096-byte TX-only ring buffer (`App/debug.c`/`HAL_App/hal_uart.c` on that branch); no RX
  DMA or RX handling exists.
- **NVIC:** DMA channel (TX complete) interrupt required; USART3 peripheral-level global
  interrupt not required for TX-only DMA operation.

### USART6 — BLE UART (RN4871)
- **Mode:** Asynchronous, 115200 baud, 8N1 — matches `wp5`'s `hal_uart.c` (previously on
  USART2).
- **DMA:** USART6_RX, **Circular** mode, byte/byte (continuous background reception into a
  256-byte ring buffer, position read via NDTR); USART6_TX, **Normal** mode, byte/byte.
  Matches old USART2 DMA config exactly, just the instance name changes.
- **NVIC:** USART6 global interrupt enabled, for IDLE-line detection (`__HAL_UART_ENABLE_IT`
  with `UART_IT_IDLE`) — used only as a wake-up hint per the driver's own comment, actual
  reads are DMA-position-polled. DMA channel interrupts for both RX and TX also required.

### USB — Custom HID (FS)
- **Mode:** Device (FS), Custom HID class.
- **Report descriptor:** `USBD_CUSTOM_HID_REPORT_DESC_SIZE` = 33 (matches the 33-byte vendor
  descriptor injected into `usbd_custom_hid_if.c` per the WP4 banner).
- **OUT report buffer:** `USBD_CUSTOM_HID_OUTREPORT_BUF_SIZE` = 64.
- **Device descriptor:** VID `0x04D8`, PID **`0xF08F`** — note this is the *final* value
  from commit `48e8562` ("set VID 0x04D8 / PID 0xF08F to match soldernerd family"), which
  superseded the placeholder `0xF08E` in the original WP4 CubeMX banner text. Manufacturer
  "soldernerd", Product "InclinationMeter", Serial "001".
- **Clock:** USB requires HSI48 + CRS locked to USB SOF (48 MHz) — see §3. Confirm this in
  the Clock Configuration tab's USB clock-source mux, separate from the main SYSCLK PLL.
- **NVIC:** USB global interrupt enabled. Old banner also said "UCPD1/UCPD2 global interrupt
  enabled" — **ASSUMED carried over faithfully, but verify it's actually needed**; this may
  just be CubeMX's default suggestion when USB FS is enabled on a part that also has UCPD,
  rather than a real requirement for a plain Custom HID device with no USB-PD negotiation.
- After regeneration, two USER CODE injections into `usbd_custom_hid_if.c` are still needed
  per the WP4 banner (report descriptor array, and `hal_usb_on_rx()` call in
  `CUSTOM_HID_OutEvent_FS`) — not a CubeMX GUI step, just a reminder these come next.

---

## 3. Clock Tree

| Setting | Value | Source |
|---|---|---|
| HSE | **8 MHz**, crystal on PF0 (OSC_IN)/PF1 (OSC_OUT) | CSV, confirmed by user. The original task brief's "16 MHz" was wrong. |
| LSE | 32.768 kHz, crystal on PC14 (OSC32_IN)/PC15 (OSC32_OUT) — **confirmed populated by user** | CSV + user confirmation. Supersedes CLAUDE.md's Open Item 2 recommendation to use LSI (that was specifically because PC14 conflicted with a PCAP04 signal on the old design — moot now that PCAP04 is gone). Use LSE for RTC. |
| PLL source | HSE | Matches old `.ioc` (`RCC_PLLSOURCE_HSE`). |
| PLL config | N=16, P/Q/R output = 64 MHz each (implies M=1, the CubeMX default) | Matches old `.ioc` exactly — carries over unchanged since HSE frequency (8 MHz) is unchanged. |
| SYSCLK | 64 MHz, source = PLLCLK | Matches old `.ioc` and CLAUDE.md §4. |
| APB1 / APB2 | **ASSUMED both = HCLK = 64 MHz** (no prescaler) | Old `.ioc` shows a single 64 MHz domain; carries over since nothing in this migration changes bus prescalers. Verify in the Clock Configuration tab regardless, since SPI2/TIM3/TIM6/I2C1 baud/prescaler math in §2 all depend on this. |
| USB clock | HSI48 (48 MHz) + CRS locked to USB Start-of-Frame | Matches old `.ioc` (`RCC.USBFreq_Value=48000000`) and CLAUDE.md §4. Configured separately from the main SYSCLK PLL — don't try to derive USB's 48 MHz from the 64 MHz PLLQ output. |
| RTC clock | **LSE, confirmed** (see above) | |

---

## 4. NVIC / Interrupt Configuration

Cross-referenced against what the driver code actually does (interrupt-driven vs. polled),
not just what the CSV's pin labels suggest.

**EXTI lines (GPIO interrupts):**

| Line | Pin | Signal | Enable? | Reasoning |
|---|---|---|---|---|
| EXTI0 | PB0 | `ENC_1B` | **Yes** | `hal_gpio.c`'s Rising/Falling dispatch is used for all encoder signal pins in WP3. |
| EXTI1 | PB1 | `ENC_2A` | **Yes** | Same. |
| EXTI2 | PB2 | `ENC_2B` | **Yes** | Same. |
| EXTI4 | PC4 | `ENC_1A` | **Yes** | Same. |
| EXTI5 | PC5 | `ENC_2SW` | **No — superseded**, see below | Was going to be EXTI5 (no line conflict existed); now `WKUP5` per user decision, see the Wake-up pins subsection below. |
| EXTI0 (via PA0) | PA0 | `ENC_1SW` | **No — cannot be**, see below | |

**`ENC_1SW` (PA0) cannot be EXTI-driven on this pinout, confirmed.** PA0 and PB0 both sit on
GPIO pin-number 0, and STM32's SYSCFG can only route one port's pin-0 to EXTI line 0 at a
time. The CSV backs this up: PB0 is explicitly labeled `GPIO_EXTI0`, while PA0 is labeled
only `SYS_WKUP1` (not `GPIO_EXTI0`) — the hardware designer's pin choice already reflects
this constraint. Same reasoning explains why `ADC_READY`/PA1 and `VBUS_SENSE`/PA2 are listed
as plain `GPIO Input` rather than EXTI, despite sharing line numbers with `ENC_2A`/PB1 and
`ENC_2B`/PB2 — those two are deliberately left off EXTI to avoid the same conflict.

**Practical effect: neither encoder switch is EXTI-driven now.** `ENC_1SW` was always going
to be polled (EXTI0 conflict, unavoidable). `ENC_2SW` had no such conflict and could have
stayed EXTI5-driven, but the user chose to configure `WKUP5` on that pin instead (see the
Wake-up pins subsection below), which — per how CubeMX's Pinout view actually works — means
giving up EXTI5 on PC5 too. Net effect: `drv_encoder.c` needs to poll both switches, not
just one. Confirmed as an accepted trade-off, not an oversight — user's assessment is these
are low-frequency signals where polling is a non-issue.

**Where that polling should live (not in the TIM6/VCOM ISR):** raised and discussed —
recommend a small `app_scheduler.c` task (same pattern as the existing `task_ui`/
`task_leds`/`task_buzzer`), not the TIM6 VCOM-toggle interrupt, even though TIM6 is
conveniently already ticking at a similar rate. Reasoning: TIM6 drives the one
safety-critical piece of this firmware (CLAUDE.md: VCOM must never stop toggling or the
display is damaged) — adding button-debounce/edge-detection logic into that ISR means more
code that must execute correctly on every tick, forever, in the interrupt that can least
afford a bug, for no latency benefit over a scheduler task. A ~20–30 ms scheduler poll gives
comparable responsiveness and happens to line up well with the switch RC filter's own
settling time (~68 kΩ/100 nF, CLAUDE.md §5.11) — debounce is close to free at that cadence.
**Not a CubeMX setting — flagged here as a decision for when `drv_encoder.c`/
`app_scheduler.c` get ported, since it falls directly out of the WKUP/EXTI choice above.**

**Other peripheral interrupts:**

| Peripheral | Interrupt(s) | Enable? | Reasoning |
|---|---|---|---|
| ADC1 | ADC1 global + DMA channel | **Yes** | `hal_adc.c` uses `HAL_ADC_Start_DMA()` + `HAL_ADC_ConvCpltCallback`. |
| I2C1 | Event + Error | **Yes** | `hal_i2c.c` relies on `HAL_I2C_MasterTxCpltCallback`/`RxCpltCallback`/`ErrorCallback`. |
| SPI2 (display) | DMA channel only | **Yes (DMA), No (SPI2 itself)** | `drv_sharp_lcd.c` uses `HAL_SPI_Transmit_DMA` + a DMA completion callback; no direct SPI2 IRQ use. |
| USART3 (debug) | DMA channel (TX) only | **Yes (DMA), No (USART3 itself)** | TX-only ring buffer, DMA-driven, no RX path exists. |
| USART6 (BLE) | USART6 global (IDLE) + DMA channels (RX/TX) | **Yes, all** | `hal_uart.c` explicitly enables `UART_IT_IDLE`, plus circular RX DMA and normal TX DMA. |
| USB | USB global (+ UCPD1/UCPD2 per old banner, verify necessity) | **Yes** | `hal_usb.c` relies on the USB device stack's ISR-driven state machine. |
| TIM6 (VCOM, new) | TIM6 global | **Yes — mandatory** | Only mechanism driving the VCOM toggle now that no hardware PWM channel is assigned to PD3. Safety-critical. |
| TIM3 (buzzer) | None | **No** | Old code starts/stops PWM directly, no timer interrupt used. |
| TIM1/TIM2 (DAC/ADC clocks) | Unknown | **Deferred** | No consuming driver yet — leave disabled until that code exists. |
| SPI1/SPI3 (ADC/DAC front end) | Unknown | **Deferred** | Same reasoning. |

**Wake-up pins (`SYS_WKUP1`/PA0, `SYS_WKUP4`/PA2, `SYS_WKUP5`/PC5) — how to configure them:**

**Correction to the previous draft:** this checklist originally claimed GPIO_Input/EXTI and
`WKUPx` are independent, layered settings in CubeMX. **User confirmed against the actual
tool that this is wrong** — the Pinout view presents `GPIO_Input`, `GPIO_EXTIx`, and
`PWR_WKUPx` as **mutually exclusive** choices for a pin's `Signal`. You pick one.

**User's decision: configure `WKUPx` now on all three pins**, even though no WP2–WP5 branch
implements Standby entry/exit yet (that's WP6) — reasoning that it costs nothing if Standby
is never entered. That's true for two of the three pins, but not the third:

| Pin | Was planned | Now (WKUPx selected) | Cost of switching to WKUPx |
|---|---|---|---|
| PA0 (`ENC_1SW`) | Plain `GPIO_Input`, polled (couldn't be EXTI0 anyway — PB0 owns that line) | `PWR_WKUP1` | **None** — it was never going to be EXTI-driven, so no Run-mode capability is lost. |
| PA2 (`VBUS_SENSE`) | Plain `GPIO_Input`, polled (old code never used EXTI here either) | `PWR_WKUP4` | **None** — same reasoning, no interrupt-driven usage was ever planned. |
| PC5 (`ENC_2SW`) | `GPIO_EXTI5`, **interrupt-driven** (no line conflict, so this was the one switch that kept old WP3 behavior) | `PWR_WKUP5` | **Real cost: loses EXTI.** `ENC_2SW` becomes polled instead of interrupt-driven — the same downgrade `ENC_1SW` already has, just for a different reason. Both switches end up polled either way. |

So "shouldn't hurt" holds for PA0 and PA2, but **PC5 does trade something away**: symmetric
interrupt-driven switches (the original WP3 design) is no longer achievable at all on this
pinout, regardless of the WKUP question — PA0 was always going to be polled (EXTI0
conflict), and now PC5 becomes polled too if `WKUP5` wins that pin's `Signal` slot. Given the
RC filter + 74HC14 already provides hardware debounce (CLAUDE.md §5.11), polling both
switches at a modest rate (e.g. same cadence as the existing `app_scheduler` UI task) is
very likely fine functionally — flagging this as a confirmed *decision*, not silently
picking it: **both encoder switches are now polled, not interrupt-driven, and `drv_encoder.c`
needs a small periodic poll for both `ENC_1SW` and `ENC_2SW` rather than relying on EXTI
callbacks for either.**

**One more thing worth checking in the GUI while you're there:** confirm that once a pin's
`Signal` is set to `PWR_WKUPx`, CubeMX still generates a normal digital `GPIO_Init()` call
for it (Input mode, not left in the POR-default Analog state) — since firmware still needs
to read PA0/PA2/PC5's live level via `HAL_GPIO_ReadPin()` for polling. If the Pinout/
Configuration view still shows GPIO pull-up/down and speed settings available for these
pins after selecting `WKUPx`, that's a good sign they're still being treated as real GPIO
inputs underneath. If those settings disappear entirely, that would be a problem worth
flagging back here before generating code.

---

## 5. Verify Before Generating Code

- [x] **Highest-priority item on this list — confirm PB11, PB12, and PA3 are real
      `ADC1_INx` channels in CubeMX — CONFIRMED in the actual tool, see §6.** `PA3=ADC1_IN3`,
      `PB11=ADC1_IN15`, `PB12=ADC1_IN16` — all three show real ADC1 channel assignments, not
      just selectable "Analog" mode. The class of mistake that caused the prototype board
      rework does not recur here.
- [x] **SPI chip-select policy — RESOLVED, see §6/§7.** SPI3/DAC fell back to software GPIO
      CS as anticipated (`PC13` can't do `SPI3_NSS`). SPI1 unaffected. SPI2/display initially
      shipped with `PD0` on hardware `SPI2_NSS`, contradicting this section's own active-low
      vs. active-HIGH analysis — since fixed in CubeMX (`PD0` is now `GPIO_Output`,
      `hspi2.Init.NSS=SPI_NSS_SOFT`) and in the CSV (row 50 now reads `GPIO Output`). All
      three SPI peripherals' chip-select config is confirmed consistent with the policy now.
- [ ] **Toolchain / IDE:** set to **CMake** (`ProjectManager.TargetToolchain=CMake`) —
      matches `master`'s current `.ioc`.
- [ ] **Library copy mode:** "Copy only the necessary library files"
      (`ProjectManager.LibraryCopy=1`) — matches `master`'s current `.ioc`. Not "copy all"
      or "use reference only."
- [ ] **Device:** confirm target is still `STM32G0B1RETx` (unchanged by this pinout
      revision).
- [x] **PB10 vs. PC10 — CONFIRMED RESOLVED.** Pin 64 was mislabeled `PB10` in an earlier
      CSV draft; corrected to `PC10` (`DAC_SCK`). `ADC_CLOCK` (PB10, pin 30) and `DAC_SCK`
      (PC10, pin 64) are distinct pins — no duplicate.
- [ ] **BME280 — RESOLVED, no CubeMX-level action needed.** It shares I2C1's existing
      pins/address space in software only; the only open question (Standard vs. Fast I2C
      mode) is a later software decision, not something that changes this `.ioc`.
- [x] **HSE frequency — CONFIRMED 8 MHz.** The "16 MHz" in the original task brief was
      wrong; §3's clock tree already reflects 8 MHz.
- [x] **LSE crystal population — CONFIRMED.** Y1 is fitted on this board revision; RTC
      clock source is LSE (§3).
- [x] **Wake-up pin (`WKUPx`) configuration — DECIDED, see §4.** User confirmed CubeMX
      treats `GPIO_Input`/`GPIO_EXTIx`/`PWR_WKUPx` as mutually exclusive per pin (corrects
      this checklist's earlier assumption). All three pins (PA0/PA2/PC5) now use `WKUPx` per
      user preference, configured now even though unused until WP6. **Still open:** verify
      in the GUI that these pins remain readable as digital GPIO inputs (not left in Analog
      mode) once `WKUPx` is selected, since PA0/PA2/PC5 all still need `HAL_GPIO_ReadPin()`
      polling in Run mode. **Accepted trade-off:** PC5 (`ENC_2SW`) loses its EXTI5 interrupt
      capability as a result — both encoder switches are now polled instead of just one.
- [x] **`ENC_1SW` (PA0) polling vs. interrupt — CONFIRMED.** User agrees it must be polled
      rather than EXTI-driven (§4); a real behavior asymmetry between the two encoder
      switches, not an oversight to fix.
- [x] **VCOM timer choice — CONFIRMED TIM6** (§2). Not specified anywhere in the CSV or
      existing code, but agreed as the right instance to use.
- [x] **Encoder pin pull configuration — CONFIRMED no internal pull.** All six encoder
      signals (`ENC_1A`/`ENC_1B`/`ENC_1SW`/`ENC_2A`/`ENC_2B`/`ENC_2SW`) are CMOS-driven by
      the encoder interface hardware, not open-collector switches — no pull-up/down needed.
      Pin table (§1) updated accordingly.
- [ ] **SPI2 baud prescaler and TIM3/TIM6 prescaler math** all assume APB1 = 64 MHz — verify
      in the Clock Configuration tab rather than trusting the carried-over old values blindly.

---

## 6. CubeMX GUI Session Results (2026-08-12)

Code has been generated from `WylerLeveltronic.ioc` against this checklist. Findings from
actually working through the GUI, checked directly against the resulting `.ioc`/generated
sources — resolved items and one open conflict.

- **SPI3/DAC chip-select — resolved, fallback confirmed needed.** `PC13` cannot be configured
  as `SPI3_NSS` in CubeMX; it's `GPIO_Output` (software CS) instead. This is exactly the
  fallback §2/§5 already allowed for ("fall back to software GPIO only if testing shows
  hardware NSS won't work") — now a confirmed fact rather than a contingency.
- **SPI2/display chip-select — RESOLVED, see §7.** `PD0` was initially generated as
  `SPI2_NSS`/`NSS_Signal_Hard_Output`, contradicting §2/§5's own conclusion that hardware NSS
  on this SPI IP is fixed active-low and incompatible with the display's active-HIGH
  `DISP_CS`. Fixed in CubeMX and the CSV — see §7 for the full resolution.
- **SPI2 direction — confirmed Transmit-Only.** `PD1`/`PD4` (`SPI2_SCK`/`SPI2_MOSI`) are
  `TX_Only_Simplex_Unidirect_Master`; no MISO pin is used, consistent with the display having
  no MISO signal in the CSV. (Note: the `.ioc`'s `SPI2.Direction` IP parameter still reads
  `SPI_DIRECTION_2LINES` despite the per-pin TX-only mode — likely just a stale/cosmetic field,
  not verified further.)
- **ADC1 channels — CONFIRMED, resolves §5's top-priority item.** `PA3=ADC1_IN3`,
  `PB11=ADC1_IN15`, `PB12=ADC1_IN16` all show real ADC1 channel assignments in CubeMX, not
  just selectable "Analog" mode. Closes out the exact risk flagged in §0/§5. **Which signal
  name goes with which of these two pins was revised in §7 below** — the channel/pin mapping
  here is still correct, only the `BATTERY_SENSE`/`TEMP_SENSE_EXT` labels swapped.
- **On-board temperature sensor — part changed to Texas Instruments TMP236**, not the LM35
  assumed throughout this checklist and the migration doc's WP2 table. No CubeMX-level
  action needed (still just an ADC1 input, `PB12`/`TEMP_SENSE`), but `Drivers_App/drv_lm35.c`
  and its `LM35_SCALE` conversion math (referenced in `docs/pinout_migration_wp2-5.md`) will
  need re-deriving against the TMP236 datasheet, and likely a rename, whenever WP2 firmware
  is ported to this pinout. Flagged here for that future work, not acted on now.

---

## 7. Hardware Pinout Finalized — Revision B (2026-08-14)

`STM32G0B1RET6_Pinout.csv` is now finalized as **Revision B, the sole hardware pinout going
forward.** User confirmed Revision A prototype boards will be manually reworked to match
Rev B rather than being treated as a separate, still-valid target — this checklist's pin
table (§1) and the migration survey (`docs/pinout_migration_wp2-5.md`) should be read against
Rev B only from this point on. Four points were checked against the finalized CSV and
resolved by user confirmation:

- **`BATTERY_SENSE` / `TEMP_SENSE_EXT` — pins are swapped from earlier drafts of this
  checklist, confirmed correct as of Rev B:**
  - `BATTERY_SENSE` = **PA3** = `ADC1_IN3` (100k/68k divider, ratio ≈0.4047)
  - `TEMP_SENSE_EXT` = **PB11** = `ADC1_IN15`
  - (`TEMP_SENSE`, on-board, is unchanged: **PB12** = `ADC1_IN16`.)
  - This reverses §0/§1/§2's earlier `BATTERY_SENSE`=PB11 / `TEMP_SENSE_EXT`=PA3 mapping.
    The `.ioc`'s channel-to-pin wiring (`PA3=IN3`, `PB11=IN15`) was already correct and
    doesn't need to change — only the *label* attached to each pin does. This matters once
    `svc_battery.c`/`drv_lm35.c`-equivalent firmware reads ADC1's DMA buffer by rank and
    needs to know which array index is which physical measurement.
- **`LED_PWR` / `LED_STATUS` — pins are swapped from earlier drafts too, confirmed correct:**
  - `LED_PWR` = **PB13**
  - `LED_STATUS` = **PB14**
  - Reverses §0/§1's earlier PB13=`LED_STATUS` / PB14=`LED_PWR` mapping. Both are plain
    `GPIO_Output`, active-HIGH, unaffected at the `.ioc` level (no CubeMX signal/mode change
    needed) — this is purely a firmware-side pin-define correction for whenever the LED
    driver is ported.
- **`CHARGE_EN` polarity — confirmed active-LOW, not active-HIGH as §1/migration-doc item 8
  assumed.** Signal renamed `!CHARGE_EN!` in the CSV: **Low = charge, High = don't charge.**
  §1 row 56's "ASSUMED initial HIGH (charging enabled)" is now known wrong — the correct
  boot-safe default (charging enabled) is initial **LOW**, the opposite of what was written.
  Flagged for whenever WP2's charge-control code is ported; no `.ioc`-level change needed
  (still plain `GPIO_Output` on PD6), this is a firmware polarity fact to get right.
- **`DISP_CS`/PD0 hardware-NSS conflict (§6) — RESOLVED.** The LS027B7DH01's `SCS` pin is
  active-HIGH (host drives it high to select the display before clocking a frame, low to
  deselect) — the reverse of the near-universal active-low SPI chip-select convention. This
  MCU's SPI hardware NSS output, when enabled (`SSOE`/`NSS_Signal_Hard_Output`), drives the
  pin low while the peripheral is enabled/transferring and releases it high when disabled —
  standard active-low behavior, hard-wired, with no polarity-invert bit on this SPI IP
  (`stm32g0xx_hal_spi.h`, confirmed no `SSIOP`-equivalent, unlike the G4/H7/L5 SPI IP that has
  one). With PD0 as hardware NSS the pin would go low exactly when the display needs it high
  (mid-transfer) and high exactly when it needs it low (idle) — inverted at the moment it
  matters, not just relabeled. **Fixed in CubeMX and confirmed in the generated code:** PD0
  is now plain `GPIO_Output` (`GPIO_MODE_OUTPUT_PP`, no pull, initial level `GPIO_PIN_RESET`
  = LOW = deselected), and `hspi2.Init.NSS` is `SPI_NSS_SOFT` in `Core/Src/spi.c` — firmware
  will need to manually drive PD0 HIGH before the SPI2 transfer and LOW after, mirroring the
  old `hal_spi_cs_assert/deassert` pattern, whenever the display driver is ported. The CSV
  (`STM32G0B1RET6_Pinout.csv` row 50) has also been corrected to `GPIO Output` with the
  reasoning noted inline ("Display uses active high CS signal. Hence SPI2_NSS cannot be
  used.") — CSV, `.ioc`, and generated code all agree now.
