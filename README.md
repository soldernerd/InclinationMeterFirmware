# InclinationMeterFirmware

Firmware for a precision electronic level instrument based on the STM32G0B1RET6.

## Status

**WP1–WP9 complete and bench-tested on `master`** (REV B hardware, WP5 skipped):

- **WP1** — Sharp Memory LCD bring-up, VCOM timer, u8g2.
- **WP2** — power rails, battery monitoring, Standby, EEPROM (per-subsystem pages).
- **WP3** — rotary encoders + buzzer + multi-screen UI (LIVE / STATUS / SETTINGS).
- **WP4** — transport-agnostic **device API v3** (fw 0.11.0; `docs/api-v3-spec.md`, generated `docs/api-reference.md`) over **three transports** — USB Custom
  HID, BLE (RN4871 Transparent UART), and a wired debug UART (USART3) — with per-transport
  non-blocking TX frame rings and a live debug-log stream. USB DFU / "Reboot to DFU" is
  parked (the STM32 ROM bootloader always bounces back to a valid app); a custom GATT
  service was descoped in favour of the Transparent UART. The API is defined once in `tools/api_spec.py` and generated
  (`python tools/gen_api.py`); Python client: `PythonTestCode/apiv3.py`.
- **WP6** — RTC (calendar on the STATUS screen + API `System status` resource 0x02, get/set),
  auto power-off after an idle timeout (EEPROM-backed, API `Settings` resource 0x1B), and a
  "Power off" menu action.
- **WP7** — AD9833 DDS waveform generator (`Drivers_App/drv_ad9833.c`): one-shot init to a
  fixed ~2604 Hz sine on `VOUT`, SPI3 + TIM1 CH4 MCLK; the DDS then free-runs on-chip.
- **WP8** — ADS131M04 4-channel simultaneous-sampling ADC front end (`Drivers_App/drv_ads131m04.c`,
  `Services/svc_signal_analysis.c`). SPI1 + TIM2 CH3 MCLK; the 20833 Hz frame read is a
  **raw-DMA** path (direct DMA1_Ch2/Ch3 + SPI1 registers, no HAL SPI state machine) polled
  from a lean TIM7 ISR at 2× the data rate. Two consumers: a single-bin DFT
  (amplitude/phase per channel, off by default — toggle via API `Commands` resource 0x01)
  and a **bulk raw-ADC capture** (API `Bulk` / `START_BULK` resource 0x00: fills a 6144-sample
  ×4-channel 24-bit-packed RAM buffer at full rate, then streams it out chunked over any
  transport — `docs/api-v2-spec.md` §4.5). Register/rate diagnostics under API `Raw data`
  resource 0x00. See `docs/wp8_ads131m04_adc.md`.
- **WP9** — Bosch BME280 environmental sensor (`Drivers_App/drv_bme280.c`): temperature /
  pressure / humidity at 1 Hz over I2C1 (shared with the EEPROM, no CubeMX change), a third
  independent temperature source. Forced mode, ×1 oversampling, hot-plug tolerant. Shown on
  the STATUS screen and readable/subscribable over the API (`Measurements` resources
  0x03–0x06: temp / pressure / humidity / fresh-flag). Bench-confirmed with a physical
  sensor (~30 °C / 973 hPa / 37 %RH, compensation math verified). See
  `docs/wp9_bme280_env_sensor.md`.

- **WP10** — displacement / tilt measurement on the analog front end (`Services/svc_displacement.c`,
  `Math/math_phasor.c`, `Math/math_window.c`): the ADS131M04 samples exactly 8 points per period of the
  2604 Hz excitation; the ISR-side hot path only adds samples into per-position int32 sums, and once
  per 64-cycle batch (40.7/s) the Q14 DFT weights, the ratio `S/(PGA*(A-B))` and the phase correction are
  applied in float. The LIVE value is a Hann-25 window (about 4 Hz) with a "doubtful" flag from a
  window-level Im(x) step-power indicator against a self-tracked quiet floor; the precision measurement
  is a sliding 2 s Hann window accepted as soon as it is clean (error after 5 s). Per-sensor k, zero
  (flip calibration) and phase calibrations are stored in EEPROM. Over the API: Measurements, Topics
  (raw per-batch delta, phasors, a gapless **phasor stream**, diagnostics), Calibrations and the
  precision command. See `docs/signal_processing.pdf` (theory and the 19 h analysis),
  `docs/wp10_displacement.md` (history) and `docs/decisions.md` (tuning rationale). Everything since fw
  0.10.64 is build- and host-test-verified but not yet bench-tested.

Builds clean (zero warnings, `-Wall -Wextra -Werror`), Debug and Release, ~RAM 76% / FLASH 35% (Debug)
(the 72 KiB raw-ADC bulk-capture buffer is half of the SRAM on its own). The `wp2`-`wp9` branch pointers
track `master`; `docs/wp2-5_rebase_status.md` is the historical record of the August branch-rebase effort.

## Hardware

| Item | Detail |
|---|---|
| MCU | STM32G0B1RET6, LQFP64, Cortex-M0+, 64 MHz, 512 KB flash, 144 KB RAM |
| Debug probe | STLINK-V3MINIE |
| Display | Sharp LS027B7DH01, 400×240 monochrome Memory LCD |
| Crystal | 8 MHz HSE → PLL → 64 MHz SYSCLK |

## Toolchain

- STM32CubeMX (project generator)
- arm-none-eabi-gcc (bundled with STM32CubeCLT)
- CMake + Ninja
- VS Code + STM32 VS Code Extension (recommended)

## Building

### From VS Code

Open the project root in VS Code with the STM32 VS Code Extension installed. Click **Build** in the status bar (or `Ctrl+Shift+B`). The extension uses the bundled `cube-cmake` wrapper.

### From the command line

With `arm-none-eabi-gcc`, `cmake` and `ninja` on `PATH` (a portable install works; see `docs/`):

```bash
cmake --preset Debug          # configures into build/Debug
cmake --build build/Debug
```

Output: `build/Debug/InclinationMeterFirmware.elf` (`cmake --preset Release` for the optimised build).

### Host unit tests

The pure-logic code (CRC, phasor combination, the display/quality windows, the TX frame ring, transfer
functions) has hardware-free tests in `tests/`; they need a native C compiler:

```bash
cd tests
make                           # gcc/clang
CC="python -m ziglang cc" make # or zig cc (pip install ziglang)
```

## Layout

```
.
├── Core/                 — CubeMX-generated HAL init (do not modify outside USER CODE)
├── Drivers/              — ST HAL/CMSIS library
├── Middlewares/ST/       — ST USB Device middleware (vendor code, do not modify)
├── USB_Device/App+Target/ — CubeMX-style USB Device glue (hand-adapted, WP4)
├── Config/               — Project-wide constants (config.h, pin_config.h)
├── HAL_App/              — Application HAL wrappers (gpio, spi, tim, systick, …)
├── Drivers_App/          — Device drivers (sharp_lcd, ads131m04, ad9833, bme280, 24lc256, …)
├── Services/             — Higher-level services (api, displacement, storage, battery, transports, …)
├── Math/                 — Pure maths, host-testable (CRC, phasor combine, windows/quality)
├── tests/                — Host unit tests (make)
├── docs/                 — Theory (signal_processing.pdf), API reference, per-WP notes, decisions.md
├── PythonTestCode/       — apiv2.py client + bench scripts;  Testing/ — archived bench tests
├── App/                  — Scheduler, UI, display, u8g2 callback, version
├── Middleware/u8g2/      — u8g2 graphics library (cloned from olikraus/u8g2)
├── system_state.{h,c}    — Global SystemState + DeviceSettings
└── WylerLeveltronic.ioc — CubeMX project (REV B)
```

CubeMX-generated code lives in `Core/` and `Drivers/`. Application code never goes inside generated files except through the `/* USER CODE BEGIN/END */` markers in [Core/Src/main.c](Core/Src/main.c).

## Architecture summary

- **Cooperative scheduler** (no RTOS) running tasks on configurable periods
- **Layered design**: App → Services → Drivers_App → HAL_App → ST HAL/LL
- **No dynamic allocation anywhere; no floats in HAL/driver code** (the Cortex-M0+ has no FPU, so float work is kept off the per-sample path)
- **u8g2** for fonts and graphics, with a custom callback that bridges to the Sharp LCD framebuffer in [drv_sharp_lcd.c](Drivers_App/drv_sharp_lcd.c)

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Companion desktop application: [soldernerd/LevelApp](https://github.com/soldernerd/LevelApp) (also GPL v3).
