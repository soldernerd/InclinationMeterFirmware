# Third-party software

This firmware is licensed under the GNU GPL v3 (`LICENSE`). It includes and builds against the following components, which
keep their own licenses. The authoritative texts are the license files shipped next to the code.

| Component | Where | License |
|---|---|---|
| STM32G0 HAL and low-level drivers (STMicroelectronics) | `Drivers/STM32G0xx_HAL_Driver/` | BSD-3-Clause (`LICENSE.txt` there) |
| CMSIS device headers for the STM32G0 (STMicroelectronics) | `Drivers/CMSIS/Device/ST/STM32G0xx/` | BSD-3-Clause (`LICENSE.txt` there) |
| CMSIS core headers (Arm) | `Drivers/CMSIS/Include/` | Apache-2.0 (`Drivers/CMSIS/LICENSE.txt`) |
| STM32 USB Device library, Custom HID class (STMicroelectronics) | `Middlewares/ST/STM32_USB_Device_Library/` | BSD-3-Clause (`LICENSE.txt` there) |
| u8g2 monochrome graphics library (Oliver Kraus) | `Middleware/u8g2/` (git submodule) | BSD-2-Clause for the code (`Middleware/u8g2/LICENSE`) |

**u8g2 fonts.** The display uses `u8g2_font_7x13_tr`, `u8g2_font_logisoso24_tr`, `u8g2_font_ncenB10_tr` and
`u8g2_font_ncenB14_tr`. u8g2's fonts come from several sources with their own terms (see the font credits on the u8g2 wiki);
the license text of u8g2 itself does not cover them. Before a commercial release, confirm the terms of exactly these four.

**Build tools** (not distributed with the firmware): Arm GNU Toolchain 14.2.Rel1, CMake, Ninja, STM32CubeMX, Python 3 with
numpy for the analysis scripts, a host C compiler for the unit tests.

The companion desktop application LevelApp (https://github.com/soldernerd/LevelApp) is a separate project.
