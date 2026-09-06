# InclinationMeterFirmware

Firmware for a precision electronic level instrument based on the STM32G0B1RET6.

## Status

**WP1–WP9 complete and bench-tested on `master`** (REV B hardware, WP5 skipped):

- **WP1** — Sharp Memory LCD bring-up, VCOM timer, u8g2.
- **WP2** — power rails, battery monitoring, Standby, EEPROM (per-subsystem pages).
- **WP3** — rotary encoders + buzzer + multi-screen UI (LIVE / STATUS / SETTINGS).
- **WP4** — transport-agnostic **device API v2** over **three transports** — USB Custom
  HID, BLE (RN4871 Transparent UART), and a wired debug UART (USART3) — with per-transport
  non-blocking TX frame rings and a live debug-log stream. USB DFU / "Reboot to DFU" is
  parked (the STM32 ROM bootloader always bounces back to a valid app); a custom GATT
  service was descoped in favour of the Transparent UART. See `docs/api-reference.md` and
  CLAUDE.md §10.
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

Builds clean (zero warnings, `-Wall -Wextra -Werror`), ~RAM 78% / FLASH 30% (the WP8 bulk
capture buffer is ~50% of SRAM on its own). Tagged `wp1-debugged` … `wp9-debugged`. The
`wp2`–`wp9` branch pointers track `master`.

The `wp10`–`wp11` branches hold sketched-but-not-bench-tested code (displacement demod,
remaining API v2 resources). Each is squash-ported onto `master` and bench-validated one at
a time. `docs/wp2-5_rebase_status.md` is the historical
record of the August branch-rebase effort (superseded by the September bench work).

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

With STM32CubeCLT installed and `arm-none-eabi-gcc`, `cmake`, and `ninja` on `PATH`:

```bash
cmake -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Output: `build/InclinationMeterFirmware.elf`.

## Layout

```
.
├── Core/                 — CubeMX-generated HAL init (do not modify outside USER CODE)
├── Drivers/              — ST HAL/CMSIS library
├── Middlewares/ST/       — ST USB Device middleware (vendor code, do not modify)
├── USB_Device/App+Target/ — CubeMX-style USB Device glue (hand-adapted, WP4)
├── Config/               — Project-wide constants (config.h, pin_config.h)
├── HAL_App/              — Application HAL wrappers (gpio, spi, tim, systick, …)
├── Drivers_App/          — Device drivers (sharp_lcd, scl3300, pcap04, …)
├── Services/             — Higher-level services (storage, calibration, …)
├── Math/                 — Filter, settling, CRC
├── App/                  — Scheduler, UI, display, u8g2 callback, version
├── Middleware/u8g2/      — u8g2 graphics library (cloned from olikraus/u8g2)
├── system_state.{h,c}    — Global SystemState + DeviceSettings
└── InclinationMeterFirmware.ioc — CubeMX project
```

CubeMX-generated code lives in `Core/` and `Drivers/`. Application code never goes inside generated files except through the `/* USER CODE BEGIN/END */` markers in [Core/Src/main.c](Core/Src/main.c).

## Architecture summary

- **Cooperative scheduler** (no RTOS) running tasks on configurable periods
- **Layered design**: App → Services → Drivers_App → HAL_App → ST HAL/LL
- **No floats, no dynamic allocation** in HAL/driver code
- **u8g2** for fonts and graphics, with a custom callback that bridges to the Sharp LCD framebuffer in [drv_sharp_lcd.c](Drivers_App/drv_sharp_lcd.c)

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Companion desktop application: [soldernerd/LevelApp](https://github.com/soldernerd/LevelApp) (also GPL v3).
