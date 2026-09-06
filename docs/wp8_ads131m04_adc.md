# WP8 — ADS131M04 ADC (Simultaneous-Sampling Front End) — Status

**Goal:** read back, via the ADS131M04, the sine wave WP7's AD9833 DAC now drives onto the
board — all 4 channels simultaneously, at a fixed multiple of the DAC's own frequency — and
compute each channel's amplitude and phase (channel 2 fixed at 0° by definition). Branches
from `wp7@10d4e19` (the tip after WP7's regen reconciliation and docs).

---

## Requirements (as specified)

- ADS131M04, SPI1, full duplex (unlike WP7's DAC, which was TX-only).
- Needs an external clock input (`CLKIN`) and a `SYNC/RESET` input in addition to SPI.
- Clock: Timer 2 CH3 on PB10, same `64 MHz / (2×6) = 5.3333...` MHz as WP7's DAC clock —
  deliberately shared so the ADC's sample rate is an exact, known multiple of the DAC's
  output frequency.
- Sample all 4 channels simultaneously at exactly 8× the DAC's output frequency: "configure
  the clock at 64MHz/12 as above and do a conversion every 256 cycles." PGA gain = 1 on all
  channels.
- All 4 inputs are sine waves at *exactly* `64 MHz / (12 × 2048)` (mid-turn clarification) —
  compute amplitude and phase of each; channel 2's phase fixed at 0 by definition. "Additional
  math may follow later" — put it in a suitable Services-layer module.

## Pin sourcing

All six pins confirmed against `STM32G0B1RET6_Pinout.csv`:

| Net | Pin | Function |
|---|---|---|
| `ADC_SCK` | PA5 | `SPI1_SCK` |
| `ADC_MISO` | PA6 | `SPI1_MISO` ← ADS131M04 `DOUT` |
| `ADC_MOSI` | PA7 | `SPI1_MOSI` → ADS131M04 `DIN` |
| `ADC_CS` | PA4 | plain `GPIO Output`, active-low — **not** `SPI1_NSS`, see CubeMX section below |
| `ADC_SYNC_RESET` | PC3 | `GPIO Output`, active-low |
| `ADC_READY` (`DRDY`) | PA1 | `GPIO Input`, active-low, **deliberately not EXTI** — see below |
| `ADC_CLOCK` (`CLKIN`) | PB10 | `TIM2_CH3` PWM |

**PA1/EXTI conflict:** `ADC_READY` would naturally want an edge interrupt, but EXTI line 1
is already committed to `PB1` (`ENC_2A`, WP3) — STM32G0's `SYSCFG_EXTICRx` can only route one
GPIO port per EXTI line number, and swapping the encoder to a different line wasn't an
option (no free line without touching other already-committed pins, and no local
STM32G0B1 reference manual was available to check for an alternative). Resolved by asking
the user directly (`AskUserQuestion`): fall back to timer-triggered polling of DRDY's raw
GPIO level instead of an edge interrupt — see below.

## Register-level design (from the datasheet)

- **SPI mode**: the datasheet's own SPI Timing Diagram states "SPI settings are CPOL = 0 and
  CPHA = 1" — SPI Mode 1. STM32 HAL naming: `CLKPolarity = SPI_POLARITY_LOW`,
  `CLKPhase = SPI_PHASE_2EDGE`. 8-bit data size (the datasheet frames everything in 8-bit
  SPI bytes, unlike the AD9833's native 16-bit words).
- **SCLK rate**: `t_c(SC)` (SCLK period) minimum is 40 ns at `2.7 V ≤ DVDD ≤ 3.6 V` (datasheet
  §6.6 Timing Requirements) → max SCLK = 25 MHz. Chose `64 MHz / 4 = 16 MHz` — comfortably
  under that ceiling, and fast enough that one full 18-byte (144-bit) frame takes ~9 µs,
  well inside the ~48 µs sample window (`1 / 20833.33 Hz`).
- **`CLOCK` register** (`0x03`, datasheet Table 8-17): `CH3_EN..CH0_EN = 1` (all four
  channels), `OSR[2:0] = 000b`. `OSR` is defined as `fMOD/fDATA` with `fMOD = fCLKIN/2`; we
  want `fDATA = fCLKIN/256` (a conversion "every 256 cycles" = 8 samples per DAC cycle
  exactly), so `OSR = (fCLKIN/2)/(fCLKIN/256) = 128` — register field `000b`, **not** a raw
  256. `PWR[1:0] = 10b` (high-resolution mode — required for `fCLKIN = 5.3333 MHz`, which
  falls in HR mode's 0.3–8.4 MHz recommended range, not low-power/very-low-power's lower
  ranges). Written value: `0x0F02` (reset default `0x0F0E` with only the `OSR` field
  cleared).
- **`GAIN1` register** (`0x04`): `0x0000` — gain = 1 on all four channels, already the
  power-on-reset default, written explicitly rather than left implicit (CLAUDE.md 7.6-
  adjacent reasoning: don't silently depend on a reset value never changing).
- **`MODE` register** (`0x02`): `0x0110` — the `0x0510` reset default with the `RESET` status
  bit (bit 10) cleared, since our own `SYNC/RESET` pulse sets it and the datasheet documents
  writing 0 as how to clear it. `WLENGTH` stays at its default `01b` (24-bit words).
- **Frame size**: with `WLENGTH = 01b`, each SPI frame is 6 words × 3 bytes = 18 bytes:
  `[response][CH0][CH1][CH2][CH3][CRC]` — fixed length regardless of command, confirmed by
  the datasheet's own frame description.
- **`SYNC/RESET` pulse**: idle high; pulse low ≥ `t_w(RSL)` = 2048 `CLKIN` cycles (~384 µs at
  5.3333 MHz) for a full device reset, then wait `t_REGACQ` (5 µs min) before communicating —
  1 ms margins used for both in the one-time boot sequence (not timing-critical, no reason to
  hand-tune).
- **Acquisition architecture**: rather than an EXTI edge on `DRDY` (unavailable — see the
  PA1/PB1 conflict above), TIM7 (a basic timer, no GPIO output) interrupts at exactly
  `64 MHz / 3072 = 20833.33 Hz` — the ADC's own sample rate, given the shared clock
  relationship makes the whole DAC→ADC signal chain deterministic. Its ISR polls `DRDY`'s
  current GPIO level; if already low, it starts a full-duplex DMA SPI transfer
  (`HAL_SPI_TransmitReceive_DMA`), reading and writing simultaneously (a `NULL`/all-zero
  command is sufficient — we only need the ADC's own conversion-result response, not to send
  any command). If `DRDY` isn't low yet (startup transient) or the previous DMA transfer
  hasn't completed, the tick is skipped and a saturating dropped-sample counter increments
  (CLAUDE.md 7.6) — self-healing, since `DRDY` will still be waiting the next tick.

## Signal analysis (single-bin 8-point DFT)

`Services/svc_signal_analysis.c` exploits the exact 8-samples-per-cycle relationship: for a
pure sine at that exact bin, `I = Σx[n]cos(2πn/8)`, `Q = Σx[n]sin(2πn/8)` over N complete
cycles gives `amplitude = (2/N)√(I²+Q²)`, `phase = atan2(Q,I)`. Channel 2's phase is
subtracted from every channel's so it reports exactly 0 by construction, per the task spec.

Split into two phases (same "no heavy work in interrupt context" reasoning as
`Services/svc_usb.c`'s `rx_handler()`):
- Per-sample accumulation runs directly in the DMA-completion interrupt context (20833.33 Hz)
  — pure `int64_t` multiply-accumulate against a fixed Q14 cos/sin table, cheap enough for
  that rate in an ISR.
- Batch finalization (`sqrtf`/`atan2f`, float) runs from `App/app_scheduler.c`'s normal task
  context (`svc_signal_analysis_update()`, `task_sensors_ms` period, ~100 ms) — never in
  interrupt context. The two communicate through a double-buffered snapshot, written once by
  the ISR when a batch completes and consumed once by the scheduler task; extra completed
  batches (batches finish every ~24.6 ms, faster than the ~100 ms consumption rate) are simply
  overwritten rather than queued, same as any other periodically-polled sensor value in this
  codebase.

Explicitly a first cut, not yet calibrated against real hardware — "additional math may
follow later" per the original request.

## Implementation

- `Config/pin_config.h` — `ADC_SCK/MISO/MOSI/CS/SYNC_RESET/READY/CLOCK_PORT/PIN` macros.
- `Config/config.h` — `ADS131M04_OSR_FIELD`, `ADS131M04_TRIGGER_TIMER_PERIOD`,
  `SIGNAL_ANALYSIS_BATCH_CYCLES`, all with the derivations above in comments.
- `HAL_App/hal_spi.h`/`.c` — `HAL_SPI_ADC` instance; new
  `hal_spi_transmit_receive_dma()` (full-duplex DMA, `HAL_SPI_ADC` only) alongside the
  existing TX-only `hal_spi_write()`/`hal_spi_write_dma()`; new `HAL_SPI_TxRxCpltCallback`
  weak override (separate from the existing `HAL_SPI_TxCpltCallback`).
- `HAL_App/hal_tim.h`/`.c` — `hal_tim_adc_clock_start()` (TIM2 CH3, mirrors WP7's
  `hal_tim_dac_clock_start()`); a generic `HalTimCallback` dispatch mechanism plus
  `hal_tim_adc_trigger_start()`/`_register_callback()` (TIM7) — keeps `HAL_App` ignorant of
  what "ADC" means, same as the rest of the layer.
- `Drivers_App/drv_ads131m04.c`/`.h` (new) — register-level init, DRDY-poll + DMA-read
  trigger, sign-extension, dropped-sample counter, `Ads131m04SampleCb` registration —
  mirrors `drv_rn4871.c`'s callback-based layering (never touches `system_state`/Services
  directly).
- `Services/svc_signal_analysis.c`/`.h` (new) — the DFT math above.
- `Core/Src/main.c` — `svc_signal_analysis_init()` called once, after `drv_ad9833_init()`
  (ADC comes after the DAC since it's sampling what the DAC drives); return value checked
  with `Error_Handler()`, same pattern as `drv_ad9833_init()`.
- `App/app_scheduler.c` — new `task_signal_analysis` entry, `task_sensors_ms` period (reuses
  the existing setting rather than adding a new EEPROM-backed field — `DeviceSettings` has no
  room left, same reasoning as `DEFAULT_TASK_UART_MS`).

## CubeMX regen — done and reconciled (2026-08-18)

The requested changes landed correctly:

- **SPI1**: `Core/Src/spi.c` confirms `CLKPhase=SPI_PHASE_2EDGE`, `NSS=SPI_NSS_SOFT`,
  `BaudRatePrescaler=SPI_BAUDRATEPRESCALER_4` (16 MHz). `HAL_SPI_MspInit()`'s SPI1 branch now
  configures only PA5/6/7 as `AF_PP` — PA4 correctly dropped out of the AF group once NSS
  went software, freeing it for `MX_GPIO_Init()` to own as a plain `GPIO_Output`, exactly as
  anticipated below.
- **TIM2**: `Core/Src/tim.c` confirms `Prescaler=1`, `CounterMode=UP`, `Period=5`, `Pulse=3`
  on Channel 3/PB10 — identical to WP7's TIM1, `64 MHz / (2×6) = 5.3333 MHz` as specified.
- **TIM7**: confirms `Prescaler=0`, `Period=3071`, and `TIM7_LPTIM2_IRQn` enabled in the NVIC
  — `64 MHz / 3072 = 20833.33 Hz` as specified. `stm32g0xx_it.c`'s
  `TIM7_LPTIM2_IRQHandler()` correctly calls `HAL_TIM_IRQHandler(&htim7)`.
- **GPIO**: PC3 (`ADC_SYNC_RESET_Pin`) confirmed `GPIO_Output`, driven `GPIO_PIN_SET` before
  `HAL_GPIO_Init()`. PA1 (`ADC_READY_Pin`) confirmed plain `GPIO_MODE_INPUT`, no EXTI.

**One real bug found in the regen, fixed the same session:** PA4 (`ADC_CS`)'s CubeMX initial
output level came back `GPIO_PIN_RESET` (LOW) instead of the idle-deasserted HIGH an
active-low CS needs — unlike the DAC's `AD9833_FSYNC` pin, which CubeMX got right on the
first WP7 regen. Left as generated, `ADC_CS` would have sat asserted from boot until
`drv_ads131m04_init()`'s first register write. Fixed defensively in
`HAL_App/hal_gpio.c`'s `hal_gpio_init()` (not CubeMX-generated, survives future regens) —
same pattern already used there for `DISP_CS`, rather than relying on getting the `.ioc`'s
GPIO Output Level checkbox right on every future regen.

As expected from the now well-established pattern, the regen **also silently broke two
things unrelated to the requested changes** — found and fixed the same session:

- `USB_Device/App/usbd_custom_hid_if.c`'s `CUSTOM_HID_ReportDesc_FS` array lost its content
  again — the **fifth** time now. Restored from git history; comment updated to "five
  separate regens now."
- `cmake/stm32cubemx/CMakeLists.txt`'s `MX_Include_Dirs` lost its hand-added `Config`/
  `HAL_App` entries again — the **third** time for this specific file. Restored, with the
  running-count note kept in the comment for next time.

**Build-verified clean after reconciliation: compiles and links with zero warnings**
(`-Wall`, clean `grep` of a full clean rebuild's output). RAM 34.6 KB (23.4%),
FLASH 133.6 KB (26.1%).

## Not yet done

- **Real-hardware verification** — same caveat as WP7: nothing here has touched real silicon.
  SPI timing margins, the DRDY-poll self-healing path, and the DFT math's actual accuracy
  are all logic/datasheet-level correctness only until flashed.
- **Amplitude calibration** — `svc_signal_analysis.c`'s mV conversion uses the ADC's
  datasheet LSB size (`2.4 V / Gain / 2^24`) directly; no empirical calibration against a
  known reference signal has been done.
- **Code review** — not yet run; should mirror WP7's 3-angle pass (register-math
  re-derivation, SPI/timer/DMA HAL usage + cross-file impact, CLAUDE.md/pin-CSV conventions).
