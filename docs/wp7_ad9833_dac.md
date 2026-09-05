# WP7 — AD9833 DAC (Waveform Generator) — Status

**Goal:** bring up the AD9833 DDS waveform generator identified in
`docs/pinout_migration_wp2-5.md`'s Open Question #2 (the previously-unidentified SPI1/SPI3
ADC/DAC front end) — the first concrete step toward giving this REV B hardware an actual
inclination-measurement front end, now that SCL3300/PCAP04 are both confirmed gone. Unlike
WP2–WP5.1, this is not a rebase of any pre-REV-B branch — REV B never had a DAC before, so
this is fresh implementation directly against the current pinout, datasheet in hand
(`sldrnrd_kicad_lib/datasheets/DAC/AD9833.pdf`).

Branches from `wp5@6bd89a2` (the tip after WP5.1's regen reconciliation). ADC-side work
(the SPI1 front end, still unidentified — see the same Open Question #2) is out of scope
here; DAC first, per the user's explicit instruction.

---

## Requirements (as specified)

- AD9833BRMZ-REEL7, SPI3 (write-only — only `MOSI`/`SCK` wired, no `MISO`; matches the
  chip's own 3-wire SCLK/SDATA/FSYNC interface, which has no return path at all).
- Needs both `3V3_EN` and `5V_EN` (the output op-amp stage needs 5V).
- MCLK fed from `TIM1_CH4`/PC11 at 64 MHz / (2×6) = 5.3333... MHz.
- Target output: ~2605 Hz sine wave, 2048 MCLK cycles per wave.
- "Once initialized, the DAC should not need any further attention except providing the
  clock signal."

## Pin sourcing

All four pins confirmed against `STM32G0B1RET6_Pinout.csv` (rows 1–4, 64):

| Net | Pin | Function |
|---|---|---|
| `DAC_Clock` | PC11 | `TMR1_CH4` — MCLK feed, **not** the SPI clock despite the name |
| `DAC_MOSI` | PC12 | `SPI3_MOSI` → AD9833 `SDATA` |
| `DAC_FSYNC` | PC13 | plain `GPIO Output` — chip select, **not** `SPI3_NSS` |
| `DAC_SCK` | PC10 | `SPI3_SCK` |

## Register-level design (from the datasheet)

- **SPI mode**: the AD9833 datasheet states data is "clocked into the AD9833 on each
  falling SCLK edge" — SPI Mode 2 (`CPOL=HIGH`, `CPHA=1EDGE`), 16-bit words, MSB first.
  `FSYNC` is a level-triggered active-low frame sync (asserted low for the whole 16-bit
  transfer, deasserted after), functionally a software chip-select — same pattern this
  codebase already uses for the display's `DISP_CS` (`App/`... `Drivers_App/drv_sharp_lcd.c`).
- **`FREQREG`**: `fOUT = FREQREG × MCLK / 2^28`. Solving for `FREQREG` at exactly 2048 MCLK
  cycles/wave: `FREQREG = 2^28 / 2048 = 2^17 = 131072` — an exact power-of-two result with
  zero rounding error, confirming 2048 was chosen deliberately for this property. At
  MCLK = 5.3333 MHz this gives `fOUT = MCLK/2048 ≈ 2604.2 Hz`, matching the ~2605 Hz target.
- **Init sequence** (datasheet Figure 7/8, "Flow Chart for AD9833 Initialization"): hold
  `RESET=1` (control word `0x2100`, `B28=1`) while loading `FREQ0` as two 14-bit halves —
  LSBs first (`0x4000`), then MSBs (`0x4008`, since `FREQREG >> 14 = 8`) — then `PHASE0=0`
  (`0xC000`; **RESET does not clear the phase/frequency/control registers**, so this must be
  written explicitly even for a zero offset, per the datasheet's own "Powering Up the
  AD9833" section), then clear `RESET` (`0x2000`). The analog output appears ~8 MCLK cycles
  later, entirely on-chip — confirming the "no further attention" requirement is the
  AD9833's own designed behavior, not an assumption.
- MCLK (`hal_tim_dac_clock_start()`) is started **before** the SPI sequence: RESET and the
  register loads are internally synchronous to MCLK, while the SPI writes themselves are
  clocked by SCLK and asynchronous to MCLK — starting MCLK first avoids any ambiguity about
  whether a register write would properly latch without a running internal clock.

## Implementation

- `Config/pin_config.h` — `AD9833_SCK/MOSI/FSYNC/CLOCK_PORT/PIN` macros.
- `Config/config.h` — `AD9833_FREQREG` (131072UL), with the derivation in a comment.
- `HAL_App/hal_spi.h`/`.c` — extended the existing `HalSpiInstance` multi-instance pattern
  (previously `HAL_SPI_DISPLAY`/`HAL_SPI_SCL3300`) with `HAL_SPI_DAC` (→ SPI3) and a
  `HAL_SPI_COUNT` sentinel, replacing the two literal `< 2U` bounds checks. `hal_spi_cs_assert/
  deassert` drive `AD9833_FSYNC` (active-low) for `HAL_SPI_DAC`, same software-CS mechanism
  the display already uses.
- `HAL_App/hal_tim.h`/`.c` — `hal_tim_dac_clock_start()`, just
  `HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4)`. No stop function — unlike the buzzer, there's
  no dynamic reconfiguration; Prescaler/Counter Period/Pulse (1/5/3) are fixed in CubeMX,
  not programmed at runtime.
- `Drivers_App/drv_ad9833.c`/`.h` (new) — the init sequence above, single
  `drv_ad9833_init()` entry point. No further public API — matches the "no further
  attention" requirement; no scheduler task was added for the same reason.
- `Core/Src/main.c` — `drv_ad9833_init()` called once, after `drv_buzzer_init()`/
  `drv_encoder_init()`, relying on `3V3_EN`/`5V_EN` already being asserted unconditionally
  by `hal_gpio_init()` (the same no-power-sequencing stopgap the buzzer and temp sensors
  already rely on — see `docs/pinout_migration_wp2-5.md` Open Question #7) and
  `MX_SPI3_Init()`/`MX_TIM1_Init()` (CubeMX-generated, above the `USER CODE` block) having
  already run. Return value checked with `Error_Handler()` on failure, matching
  `hal_adc_init()`'s established pattern.

## Code review (2026-08-18)

3 parallel angles: independent register-math re-derivation against the datasheet, SPI/timer
HAL usage + cross-file impact, and CLAUDE.md/pin-CSV conventions. Results:

- **Register math — fully clean, no errors.** Every bit position (`CTRL_B28`=D13,
  `CTRL_RESET`=D8), register-address constant (`FREQ0_WRITE`=0x4000, `PHASE0_WRITE`=0xC000),
  the `AD9833_FREQREG` derivation, the LSB/MSB write order and split, and the overall init
  sequence (vs. the datasheet's Figure 7/8 flowchart) were independently re-derived from the
  datasheet and matched exactly.
- **Pin assignments — fully clean, no errors.** All four `AD9833_*` macros in
  `Config/pin_config.h` cross-checked against `STM32G0B1RET6_Pinout.csv` — nothing transposed
  or invented.
- **One real finding, fixed: CLAUDE.md 7.6 violation.** `drv_ad9833_init()` was `void` and
  silently discarded every SPI write failure — no return-value check, no escalation,
  inconsistent with `drv_encoder_init()`'s established `DrvStatus`-returning pattern in the
  same layer. Fixed: `hal_spi_write()` now returns `DrvStatus` (propagating
  `HAL_SPI_Transmit`'s result, matching `hal_uart_write()`'s already-established pattern),
  `drv_ad9833_init()` now returns `DrvStatus` and checks every write, and `main.c` checks
  the return with `Error_Handler()`.
- **One documentation finding, fixed:** `main.c`'s comment claimed `MX_TIM1_Init()` "already
  run" in present tense while it didn't exist yet — reworded once the regen below actually
  landed and made the claim true.
- The SPI3-config-mismatch and missing-`htim1` findings both angles also (re-)surfaced were
  already known — see the CubeMX section below, not new defects.

## CubeMX regen — done and reconciled (2026-08-18)

The three requested changes landed exactly right:
- **TIM1**: `Core/Src/tim.c` confirms `Prescaler=1`, `CounterMode=UP`, `Period=5`, `Pulse=3`
  on Channel 4 — `64 MHz / (2×6) = 5.3333 MHz` as specified.
- **SPI3**: `Core/Src/spi.c` confirms `DataSize=SPI_DATASIZE_16BIT`,
  `CLKPolarity=SPI_POLARITY_HIGH`, `CLKPhase=SPI_PHASE_1EDGE` (Mode 2, matching the AD9833's
  falling-edge capture), `BaudRatePrescaler=/4` (16 MHz).
- **GPIO**: PC13 (CubeMX label `DAC_SYNC_Pin`, close enough to `DAC_FSYNC` — cosmetic only)
  confirmed `GPIO_Output`, driven `GPIO_PIN_SET` (idle-deasserted High) before
  `HAL_GPIO_Init()`, matching `hal_gpio.c`'s established pattern for other custom pins.

As expected from the established pattern (`docs/wp2-5_rebase_status.md`'s WP5.1 update
already catalogued this exact failure mode twice), the regen **also silently broke two
things unrelated to the requested changes** — found and fixed the same session:
- `USB_Device/App/usbd_custom_hid_if.c`'s `CUSTOM_HID_ReportDesc_FS` array lost its content
  again (down to one byte) — the fourth time now. Restored; comment updated to "four
  separate regens."
- `cmake/stm32cubemx/CMakeLists.txt`'s `MX_Include_Dirs` lost its hand-added `Config`/
  `HAL_App` entries again — the second time for this specific file. Restored.

**Build-verified clean after reconciliation: compiles and links with zero warnings.**
RAM 34.2 KB (23.2%), FLASH 122.2 KB (23.3%).

## Not yet done

- **Real-hardware verification** — nothing in this project has touched real silicon yet;
  this is logic-level/datasheet-level correctness only, now confirmed to at least compile,
  link, and match the datasheet's own math. The actual DAC output frequency, waveform
  shape, and SPI timing margins should be scoped on a real board once flashed.
- **ADC front end** (SPI1) — still unidentified (`docs/pinout_migration_wp2-5.md` Open
  Question #2), separate future work, deliberately out of scope for this DAC-only pass.
