# InclinationMeterFirmware

Firmware for a precision electronic level instrument based on the STM32G0B1RET6.

## Status

**WP1–WP7 complete and on `master`** (REV B hardware), all bench-tested on real silicon
(WP5 skipped):

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

Builds clean (zero warnings, `-Wall -Wextra -Werror`), ~RAM 27% / FLASH 25%. Tagged
`wp1-debugged` … `wp7-debugged`. The `wp2`–`wp7` branch pointers track `master`.

The `wp8`–`wp11` branches hold sketched-but-not-bench-tested sensor drivers
(ADS131M04, BME280, displacement demod). Each is squash-ported onto `master` and
bench-validated one at a time. `docs/wp2-5_rebase_status.md` is the historical record of
the August branch-rebase effort (superseded by the September bench work).

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
