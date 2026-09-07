# dfu_flash.ps1 - reflash the InclinationMeter over USB DFU (no ST-Link) and
# restore normal application boot.
#
# Use this after the device has been put into DFU with "Reboot to DFU"
# (menu action, or API EXECUTE Commands/0x05). That path sets the nBOOT0
# option byte to 0, so the device boots the ROM bootloader on EVERY reset
# until nBOOT0 is set back to 1. This script does both: writes the new
# firmware, then restores nBOOT0=1 (which resets the device into the app).
#
#   .\dfu_flash.ps1                       # write build\Debug\...hex over DFU
#   .\dfu_flash.ps1 -Elf path\to\fw.elf   # convert + write a specific image
#   .\dfu_flash.ps1 -Hex path\to\fw.hex   # write a specific .hex
#
# The board must enumerate as "STM32 Bootloader" (VID 0x0483 / PID 0xDF11).
# If it doesn't after "Reboot to DFU", unplug/replug the USB cable once -
# some hosts don't re-enumerate a bus-powered device across the fast
# option-byte-reload reset.

param(
    [string]$Elf = "build\Debug\InclinationMeterFirmware.elf",
    [string]$Hex
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repo

$armRoot = "C:\Users\lfaes\arm-dev-tools"
$cli = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
if (-not (Test-Path $cli)) { throw "STM32_Programmer_CLI.exe not found at $cli" }

if (-not $Hex) {
    if (-not (Test-Path $Elf)) { throw "image not found: $Elf" }
    $Hex = [System.IO.Path]::ChangeExtension($Elf, ".hex")
    & "$armRoot\arm\bin\arm-none-eabi-objcopy.exe" -O ihex $Elf $Hex
    Write-Host "==> $Hex" -ForegroundColor DarkGray
}
if (-not (Test-Path $Hex)) { throw "hex not found: $Hex" }

$dfu = & $cli -l usb 2>&1 | Select-String -Pattern "Device Index\s+: (USB\d+)"
if (-not $dfu) {
    throw "no USB DFU device found. Is it enumerated as 'STM32 Bootloader'? Try an unplug/replug."
}
$port = $dfu.Matches[0].Groups[1].Value
Write-Host "==> DFU device on $port" -ForegroundColor Cyan

# Step 1: firmware only. Stay in DFU (no -ob, no -rst) so the write is
# verified against a still-connected device.
Write-Host "==> writing $Hex" -ForegroundColor Cyan
& $cli -c port=$port -w $Hex -v
if ($LASTEXITCODE -ne 0) { throw "DFU firmware write failed (rc=$LASTEXITCODE). Device is still in DFU - retry, or recover over SWD with .\flash.ps1" }

# Step 2: restore nBOOT0=1. This makes the next reset boot the application,
# so the device leaves DFU - CubeProgrammer's post-op DFU reconnect then
# times out. That timeout is expected; verify the real state over SWD below.
Write-Host "==> restoring nBOOT0=1 (device will leave DFU)" -ForegroundColor Cyan
& $cli -c port=$port -ob nBOOT_SEL=1 nBOOT0=1 2>&1 |
    Select-String -Pattern "Option Bytes|programmed|nBOOT|Error" | ForEach-Object { "   $_" }

Start-Sleep -Seconds 2
Write-Host "==> verifying over SWD" -ForegroundColor Cyan
& $cli -c port=SWD mode=UR -ob displ 2>&1 | Select-String -Pattern "RDP|nBOOT0\s|nBOOT_SEL" | ForEach-Object { "   $_" }
& $cli -c port=SWD mode=UR --hardRst 2>&1 | Out-Null
Write-Host "==> done. If nBOOT0 = 0x1 above, the app is running again." -ForegroundColor Green
