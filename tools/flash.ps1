# Flash Hall_Senser onto GD32E230G6 with OpenOCD (CMSIS-DAP / SWD).
# Prefer GigaDevice OpenOCD (`gd32e23x`). xPack/stock OpenOCD uses
# tools/openocd-gd32e230-stm32f1x.cfg (32 KB, no mass_erase).
# Usage:
#   .\tools\flash.ps1
#   .\tools\flash.ps1 -BuildBeforeFlash
#   .\tools\flash.ps1 -Preset debug
#   .\tools\flash.ps1 -ElfFile 'D:\path\Hall_Senser.elf'
#   .\tools\flash.ps1 -EraseConfig
# -EraseConfig 只擦最后 1 KB（0x08007C00）里的设备号、模式、基线和宽度，
# 程序本身照常烧录。擦完上电回到角度编码器、设备号 1、115200。

param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'release',

    [string]$ElfFile = '',
    [string]$OpenOcdExe = '',
    [string]$OpenOcdScripts = '',
    [string]$ConfigFile = '',

    [Alias('Build')]
    [switch]$BuildBeforeFlash,
    [switch]$NoResetRun,
    [switch]$EraseConfig
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot

function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments
    )
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Get-FirstExisting {
    param([string[]]$Paths)
    foreach ($p in $Paths) {
        if ([string]::IsNullOrWhiteSpace($p)) { continue }
        $hits = @(Get-Item -Path $p -ErrorAction SilentlyContinue)
        if ($hits.Count -gt 0) {
            return ($hits | Sort-Object FullName -Descending | Select-Object -First 1).FullName
        }
    }
    return $null
}

function Find-OpenOcdScriptsDir {
    param([string]$HomeDir)
    foreach ($rel in @('openocd\scripts', 'share\openocd\scripts', 'scripts')) {
        $d = Join-Path $HomeDir $rel
        if (Test-Path -LiteralPath $d) { return $d }
    }
    return $null
}

function Test-OpenOcdHasGd32E23x {
    param(
        [string]$Exe,
        [string]$ScriptsDir
    )
    if ($Exe -match 'com\.gd\.tools') { return $true }
    if (-not $ScriptsDir) { return $false }
    return (Test-Path -LiteralPath (Join-Path $ScriptsDir 'target\openocd_gdlink_gd32e23x.cfg'))
}

function Find-OpenOcd {
    if ($env:HALL_OPENOCD_EXE -and (Test-Path -LiteralPath $env:HALL_OPENOCD_EXE)) {
        $homeDir = Split-Path -Parent (Split-Path -Parent $env:HALL_OPENOCD_EXE)
        $scripts = $env:HALL_OPENOCD_SCRIPTS
        if (-not $scripts) { $scripts = Find-OpenOcdScriptsDir $homeDir }
        return [pscustomobject]@{
            Exe     = $env:HALL_OPENOCD_EXE
            Scripts = $scripts
            HasGd32 = Test-OpenOcdHasGd32E23x $env:HALL_OPENOCD_EXE $scripts
        }
    }

    $gdHome = Get-FirstExisting @(
        'D:\GD32EB_v1.5.11_Rel\GD32EB_v1.5.11_Rel\GD32EB\plugins\com.gd.tools.openocd_*\Tools\OpenOCD'
        'D:\GD32EB*\*\GD32EB\plugins\com.gd.tools.openocd_*\Tools\OpenOCD'
        'C:\GD32EB*\*\GD32EB\plugins\com.gd.tools.openocd_*\Tools\OpenOCD'
    )
    if ($gdHome -and (Test-Path -LiteralPath (Join-Path $gdHome 'bin\openocd.exe'))) {
        $scripts = Join-Path $gdHome 'openocd\scripts'
        $exe = Join-Path $gdHome 'bin\openocd.exe'
        return [pscustomobject]@{
            Exe     = $exe
            Scripts = $scripts
            HasGd32 = $true
        }
    }

    $exe = Get-FirstExisting @(
        'D:\ClionProject\YFOCV3_LITE\tools\openocd\bin\openocd.exe'
        'D:\ClionProject\YFOCV3_LITE\tools\openocd\*\bin\openocd.exe'
    )
    if (-not $exe) {
        $cmd = Get-Command openocd -ErrorAction SilentlyContinue
        if ($cmd) { $exe = $cmd.Source }
    }
    if ($exe) {
        $homeDir = Split-Path -Parent (Split-Path -Parent $exe)
        $scripts = Find-OpenOcdScriptsDir $homeDir
        return [pscustomobject]@{
            Exe     = $exe
            Scripts = $scripts
            HasGd32 = Test-OpenOcdHasGd32E23x $exe $scripts
        }
    }
    return $null
}

$ocd = Find-OpenOcd
if ($OpenOcdExe -eq '') {
    if ($ocd) { $OpenOcdExe = $ocd.Exe }
}
if ($OpenOcdScripts -eq '') {
    if ($ocd) { $OpenOcdScripts = $ocd.Scripts }
}

$hasGd32 = $false
if ($ocd) { $hasGd32 = [bool]$ocd.HasGd32 }
if ($OpenOcdExe -or $OpenOcdScripts) {
    $hasGd32 = Test-OpenOcdHasGd32E23x $OpenOcdExe $OpenOcdScripts
}

if ($ConfigFile -eq '') {
    if ($hasGd32) {
        $ConfigFile = Join-Path $PSScriptRoot 'openocd-gd32e230.cfg'
    }
    else {
        $ConfigFile = Join-Path $PSScriptRoot 'openocd-gd32e230-stm32f1x.cfg'
    }
}

if ($ElfFile -eq '') {
    $ElfFile = Join-Path $Repo "firmware\build\$Preset\Hall_Senser.elf"
}

if ($BuildBeforeFlash) {
    $buildScript = Join-Path $PSScriptRoot 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildScript)) {
        throw "Build script not found: $buildScript"
    }
    & $buildScript -Preset $Preset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not $OpenOcdExe -or -not (Test-Path -LiteralPath $OpenOcdExe)) {
    throw "OpenOCD executable not found. Install GD32EB OpenOCD or pass -OpenOcdExe."
}
if (-not $OpenOcdScripts -or -not (Test-Path -LiteralPath $OpenOcdScripts)) {
    throw "OpenOCD scripts directory not found. Pass -OpenOcdScripts."
}
if (-not (Test-Path -LiteralPath $ConfigFile)) {
    throw "OpenOCD config not found: $ConfigFile"
}
if (-not (Test-Path -LiteralPath $ElfFile)) {
    throw "ELF file not found: $ElfFile. Run .\tools\build.ps1 -Preset $Preset first."
}

$resolvedElf = (Resolve-Path -LiteralPath $ElfFile).Path
$elfForOpenOcd = $resolvedElf.Replace('\', '/')

# Page-erase used pages, then verify. Do not stm32f1x mass_erase.
# -EraseConfig also wipes the last 1 KB parameter page, which program never touches.
$commands = @(
    'tcl port disabled',
    'gdb port disabled'
)
$program = "program `"$elfForOpenOcd`" verify"
if (-not $NoResetRun) {
    $program += ' reset'
}
$program += ' exit'
if ($EraseConfig) {
    $commands += "init; reset halt; flash erase_address 0x08007C00 0x400; $program"
} else {
    $commands += $program
}

$ocdArgs = @(
    '-s', $OpenOcdScripts,
    '-f', $ConfigFile
)
foreach ($command in $commands) {
    $ocdArgs += @('-c', $command)
}

Write-Host "Target: GD32E230G6U6TR"
Write-Host "Preset: $Preset"
Write-Host "ELF: $resolvedElf"
Write-Host "OpenOCD: $OpenOcdExe"
if ($hasGd32) {
    Write-Host "Flash: gd32e23x program+verify (no mass_erase)"
} else {
    Write-Host "Flash: stm32f1x fallback 32 KB program+verify (no mass_erase)"
}
if ($EraseConfig) {
    Write-Host "Config: erase 0x08007C00 (1 KB). Next boot is encoder, id 1, 115200."
}

Invoke-Native -Exe $OpenOcdExe -Arguments $ocdArgs
Write-Host "OK: flashed $resolvedElf"
