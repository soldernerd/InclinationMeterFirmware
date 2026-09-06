# flash.ps1 - build (optional) + flash the InclinationMeter firmware over ST-Link/SWD.
#
# Uses STM32CubeProgrammer's CLI with connect-under-reset + run-after, which
# replaces the manual "reset + halt + run" dance in the CubeProgrammer GUI.
#
#   .\flash.ps1            # flash build\Debug\InclinationMeterFirmware.elf as-is
#   .\flash.ps1 -Build     # cmake --build first, then flash
#   .\flash.ps1 -Elf path  # flash a specific .elf/.hex
#
# NOTE: the STM32CubeProgrammer GUI holds the ST-Link exclusively. If it is
# open, hit "Disconnect" there first (you can leave the window open).

param(
    [switch]$Build,
    [string]$Elf = "build\Debug\InclinationMeterFirmware.elf"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repo

$armRoot = "C:\Users\lfaes\arm-dev-tools"
$env:Path = "$armRoot\arm\bin;$armRoot\cmake-4.4.3-windows-x86_64\bin;$armRoot\ninja;$env:Path"

if ($Build) {
    Write-Host "==> cmake --build build/Debug" -ForegroundColor Cyan
    cmake --build build/Debug
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path $Elf)) { throw "firmware image not found: $Elf" }

# Emit a .hex next to the .elf too (handy to hand over / archive).
if ($Elf.ToLower().EndsWith(".elf")) {
    $hex = [System.IO.Path]::ChangeExtension($Elf, ".hex")
    & "$armRoot\arm\bin\arm-none-eabi-objcopy.exe" -O ihex $Elf $hex
    if ($LASTEXITCODE -eq 0) { Write-Host "==> wrote $hex" -ForegroundColor DarkGray }
}

$cli = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
if (-not (Test-Path $cli)) { throw "STM32_Programmer_CLI.exe not found at $cli" }

Write-Host "==> flashing $Elf" -ForegroundColor Cyan
# mode=UR (connect under reset) copes with the target being asleep / in Standby,
# where a plain hot-plug SWD attach fails. -rst runs the new image afterwards, so
# no manual halt/run in the GUI. If NRST is wired to the ST-Link, adding
# "reset=HWrst" makes the under-reset connect more reliable still.
& $cli -c port=SWD mode=UR -d $Elf -v -rst
exit $LASTEXITCODE
