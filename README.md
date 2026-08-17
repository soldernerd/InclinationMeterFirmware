# InclinationMeterFirmware

Firmware for a precision electronic level instrument based on the STM32G0B1RET6.

## Status

**WP1–WP5 implemented on the `wp5` branch** (REV B hardware): display bring-up, power
management/battery monitoring/EEPROM storage, the rotary-encoder + buzzer + multi-screen UI
local interaction layer, a transport-agnostic device API (USB Custom HID and BLE) with a
single-shot measurement state machine, and BLE connectivity via an RN4871 module (UART
AT-command state machine, transparent-mode data path). Builds clean (zero warnings,
`-Wall -Wextra -Werror`). WP1–WP5 have all been through multiple rounds of structured code
review — WP4 through two full passes plus reconciliation against two real CubeMX
regenerations, which caught real regen-induced regressions (including one that would have
hard-hung the MCU on first encoder touch); WP5 through one full pass, which caught a
boot-time UART interrupt storm that would have hung the MCU almost immediately after boot,
plus two layering violations and several silent-failure gaps — see `docs/wp2-5_rebase_status.md`
for the complete findings list. **Not yet flashed to real hardware** — see that same doc for
the full per-branch history and what's still unverified against real silicon. `master` itself
currently reflects WP1 only; `wp2`/`wp3`/`wp4`/`wp5` are feature branches pending their own
review/merge.

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
