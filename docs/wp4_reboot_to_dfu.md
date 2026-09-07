# WP4 — "Reboot to DFU" — Status: SOLVED (option-byte method)

**Goal:** let the device be reflashed over its own USB (no ST-Link) by rebooting
into the STM32 ROM bootloader's USB DFU interface (VID `0x0483` / PID `0xDF11`,
D+/D- on PA11/PA12).

History: parked through fw 0.4.20-0.4.36 and revisited on the `dfu` branch. All
**software-jump** and **PROGEMPTY** variants were retried, this time from the clean
pre-`HAL_Init()` reset state (`.noinit` request flag + jump as the first statement
of `main()`). Bench outcome on REV B hardware:

| approach | result |
|---|---|
| in-app jump to `0x1FFF0000` (fw 0.4.x) | never left the app |
| clean-reset jump from top of `main()` (fw 0.9.8-0.9.9) | **reliably reaches the bootloader** (`PC` verified at `0x1FFF5BA4`), but with a valid app present and no hardware BOOT0 the bootloader hands control straight back — USB DFU never enumerates |
| same, after `-e all` mass-erase | still no DFU: the "flash is empty" check that would make the bootloader stay is only re-sampled at a **power-on reset**, which firmware cannot produce (`NVIC_SystemReset`, Standby exit, and OBL_LAUNCH all count as system resets, not POR) |
| **`nBOOT0` option byte = 0 + option-byte reload** (fw 0.9.11+) | **works** — device boots the ROM bootloader and *stays*; USB DFU enumerates as `STM32 Bootloader` |

So the only entry the G0 bootloader treats as "stay here and wait for a host" is a
boot-configuration selection via the option bytes. `hal_dfu_enter_bootloader()`
(`HAL_App/hal_dfu.c`) programs `nBOOT_SEL = 1` (boot source is the `nBOOT0` bit, not
the PA14/BOOT0 pin) and `nBOOT0 = 0` (that source selects system memory), then calls
`HAL_FLASH_OB_Launch()` — an option-byte reload, i.e. a reset that re-reads the boot
configuration. It does not return.

## Trigger

- **API:** `EXECUTE` / Commands (`0x1`) / resource `0x05` (`API2_RES_CMD_REBOOT_DFU`),
  0-byte payload. Any transport. Sends `OK`, drains, then goes offline.
- **Menu:** SETTINGS -> "Reboot to DFU" action row (encoder-1 press to confirm).

Before the reload the firmware drops the USB D+ pull-up (`hal_usb_detach()` ->
`USBD_Stop`) and waits 400 ms so a USB host sees a clean disconnect.

## Recovery — NOT automatic

Once triggered, **every** reset boots the ROM bootloader. A power-cycle does **not**
bring the application back. Recovery is a reflash that also restores `nBOOT0 = 1`,
in one operation:

```
.\dfu_flash.ps1                    # writes build\Debug\...hex over DFU, restores nBOOT0=1
```

or by hand:

```
STM32_Programmer_CLI -c port=USB1 -w firmware.hex
STM32_Programmer_CLI -c port=USB1 -ob nBOOT_SEL=1 nBOOT0=1
```

(The `-ob` step makes the device leave DFU, so STM32CubeProgrammer's own post-op
DFU reconnect then times out — that error is expected; verify the real state with
`STM32_Programmer_CLI -c port=SWD mode=UR -ob displ`.)

SWD always works too: `.\flash.ps1` rewrites the app and, because a fresh image is
built with `nBOOT0` untouched at its default, ST-Link programming leaves the option
byte at whatever it was — so if the board is stuck in DFU, use `dfu_flash.ps1` or add
`-ob nBOOT0=1` to a manual `STM32_Programmer_CLI` SWD call.

## Known rough edge — host re-enumeration

After the option-byte-reload reset the device disconnects and comes back as a
*different* USB device (DFU instead of the app's HID). Some hosts (observed on
Windows 10) do not re-enumerate a bus-powered device across that fast identity
change and show nothing. Fix: **tap NRST once, or unplug/replug the USB cable
once** — the DFU device then appears within ~3 s. A hardware reset always works;
this is the common "double-tap reset" seen with software-triggered DFU across the
STM32 range, not specific to this design.

`dfu_test.py` triggers the command and polls for the DFU device, printing the
NRST/replug hint if it doesn't show.

## Files

- `HAL_App/hal_dfu.c` / `.h` — `hal_dfu_enter_bootloader()`
- `HAL_App/hal_usb.c` / `.h` — `hal_usb_detach()` (D+ pull-up down)
- `Services/svc_api.c` / `.h` — Commands resource `0x05`
- `App/app_ui.c` — SETTINGS "Reboot to DFU" -> `hal_dfu_enter_bootloader()`
- `dfu_flash.ps1` — USB-DFU reflash + `nBOOT0=1` restore
- `PythonTestCode/dfu_test.py` — trigger + verify
