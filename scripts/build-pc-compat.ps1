# Builds the 32-bit PC compatibility layer (native/pc-compat -> build/pc-compat/d3d8.dll) with the pinned llvm-mingw.
# The in-game overlay uses Dear ImGui (MIT), pinned below and fetched into build/pc-compat-deps (never committed).
param(
    [string]$ToolsRoot = (Join-Path $env:USERPROFILE 'Tools'),
    [string]$Out = (Join-Path $PSScriptRoot '..\build\pc-compat')
)
$ErrorActionPreference = 'Continue'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$manifest = Get-Content (Join-Path $repo 'toolchains\toolchains.json') -Raw | ConvertFrom-Json
$llvm = $manifest.tools | Where-Object { $_.id -eq 'llvm-mingw' }
$bin = Join-Path $ToolsRoot $llvm.bin
$cxx = Join-Path $bin 'i686-w64-mingw32-clang++.exe'
if (-not (Test-Path $cxx)) { Write-Error "i686 clang++ not found at $cxx (run scripts/setup-toolchains.ps1)"; exit 1 }
$env:PATH = "$bin;$env:PATH"

# Dear ImGui: the same release the PS2 runners use (docs/THIRD_PARTY.md), checked by commit
$imguiTag = 'v1.92.7-docking'
$imguiCommit = 'b1bcb12a624af7509894c8e77dd47416997777fa'
$deps = Join-Path $repo 'build\pc-compat-deps'
$imgui = Join-Path $deps 'imgui'
$have = if (Test-Path (Join-Path $imgui '.git')) { (& git -C $imgui rev-parse HEAD 2>$null) } else { '' }
if ($have -ne $imguiCommit) {
    if (Test-Path $imgui) { Remove-Item -Recurse -Force $imgui }
    New-Item -ItemType Directory -Force $deps | Out-Null
    & git clone --quiet --depth 1 --branch $imguiTag https://github.com/ocornut/imgui.git $imgui
    $have = (& git -C $imgui rev-parse HEAD 2>$null)
    if ($have -ne $imguiCommit) { Write-Error "Dear ImGui $imguiTag is $have, expected $imguiCommit; refusing to build"; exit 1 }
}

New-Item -ItemType Directory -Force $Out | Out-Null
$src = Join-Path $repo 'native\pc-compat'
$sources = @(Get-ChildItem (Join-Path $src 'src\*.cpp') | ForEach-Object { $_.FullName })
$sources += @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends\imgui_impl_win32.cpp') |
    ForEach-Object { Join-Path $imgui $_ }
$dll = Join-Path (Resolve-Path $Out) 'd3d8.dll'
# static runtime: the DLL must load next to a 2002 game without any extra DLLs
$argv = @('-std=c++17', '-O2', '-Wall', '-Wno-unknown-pragmas', '-shared', '-static',
    "-I$imgui", "-I$(Join-Path $imgui 'backends')", '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',
    '-o', $dll) + $sources + @((Join-Path $src 'd3d8.def'),
    '-luser32', '-lgdi32', '-ladvapi32', '-lwinmm', '-lole32', '-lwindowscodecs', '-luuid', '-ldxguid', '-limm32',
    '-ldwmapi', '-Wl,--enable-stdcall-fixup')
& $cxx @argv
if ($LASTEXITCODE -ne 0) { Write-Error "build failed ($LASTEXITCODE)"; exit $LASTEXITCODE }
if (-not (Test-Path $dll)) { Write-Error 'build reported success but d3d8.dll is missing'; exit 1 }
Write-Host "built $dll"
