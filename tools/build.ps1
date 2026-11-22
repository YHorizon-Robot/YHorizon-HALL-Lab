# Build Hall_Senser on Windows (CMake + Ninja, GD32EB ARM GCC).
# Usage:
#   .\tools\build.ps1
#   .\tools\build.ps1 -Preset debug
#   .\tools\build.ps1 -Flash

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'release',

    [switch]$Flash
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$Firmware = Join-Path $Repo 'firmware'

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

function Find-Cmake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return Get-FirstExisting @(
        'C:\Program Files\JetBrains\CLion *\bin\cmake\win\x64\bin\cmake.exe'
        'D:\Program Files\JetBrains\CLion *\bin\cmake\win\x64\bin\cmake.exe'
        'D:\JetBrains\CLion *\bin\cmake\win\x64\bin\cmake.exe'
        "$env:LOCALAPPDATA\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe"
        'C:\Program Files\CMake\bin\cmake.exe'
        'D:\ClionProject\YFOCV3_LITE\tools\cmake\bin\cmake.exe'
    )
}

$cmake = Find-Cmake
if (-not $cmake) {
    throw "cmake not found. Install CMake or CLion."
}
Write-Host "cmake: $cmake"

Push-Location $Firmware
try {
    & $cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    & $cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
    Write-Host "OK: $Firmware\build\$Preset\Hall_Senser.elf"
}
finally {
    Pop-Location
}

if ($Flash) {
    $flashScript = Join-Path $PSScriptRoot 'flash.ps1'
    & $flashScript -Preset $Preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
