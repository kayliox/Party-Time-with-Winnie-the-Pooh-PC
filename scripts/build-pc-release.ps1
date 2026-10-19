# Builds a pc-compat game's release in one step: the compatibility layer (build/pc-compat/d3d8.dll), then the setup
# program that embeds it (build/pc-installer/<name>.exe). Needs the pinned llvm-mingw (scripts/setup-toolchains.ps1
# -Only llvm-mingw); fetches the pinned Dear ImGui on first use.
param(
    [string]$Game = 'PC-DPTWP-UK',
    [string]$Version = '1.0.0'
)
$ErrorActionPreference = 'Stop'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-pc-compat.ps1')
if ($LASTEXITCODE -ne 0) { throw "building the compatibility layer failed ($LASTEXITCODE)" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-pc-installer.ps1') -Game $Game -Version $Version
if ($LASTEXITCODE -ne 0) { throw "building the setup failed ($LASTEXITCODE)" }
