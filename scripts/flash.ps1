# Flash Hall_Senser onto GD32E230G6 with OpenOCD (CMSIS-DAP / SWD).
# Prefer GigaDevice OpenOCD (`gd32e23x`). xPack/stock OpenOCD uses
# scripts/openocd-gd32e230-stm32f1x.cfg (32 KB, no mass_erase).
# Usage:
#   .\scripts\flash.ps1
#   .\scripts\flash.ps1 -BuildBeforeFlash
#   .\scripts\flash.ps1 -Preset release
#   .\scripts\flash.ps1 -ElfFile 'D:\path\Hall_Senser.elf'

param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'debug',

    [string]$ElfFile = '',
    [string]$OpenOcdExe = '',
    [string]$OpenOcdScripts = '',
    [string]$ConfigFile = '',

    [Alias('Build')]
    [switch]$BuildBeforeFlash,
    [switch]$NoResetRun
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot

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
    $ElfFile = Join-Path $Root "build\$Preset\Hall_Senser.elf"
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
    throw "ELF file not found: $ElfFile. Run .\scripts\build.ps1 -Preset $Preset first."
}

$resolvedElf = (Resolve-Path -LiteralPath $ElfFile).Path
$elfForOpenOcd = $resolvedElf.Replace('\', '/')

# Page-erase used pages, then verify. Do not stm32f1x mass_erase.
$commands = @(
    'tcl port disabled',
    'gdb port disabled'
)
if ($NoResetRun) {
    $commands += "program `"$elfForOpenOcd`" verify exit"
} else {
    $commands += "program `"$elfForOpenOcd`" verify reset exit"
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

Invoke-Native -Exe $OpenOcdExe -Arguments $ocdArgs
Write-Host "OK: flashed $resolvedElf"
