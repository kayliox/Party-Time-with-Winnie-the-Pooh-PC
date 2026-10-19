# Installs the pinned host toolchains from toolchains/toolchains.json (no admin): download, verify SHA-256, extract.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/setup-toolchains.ps1 [-Only llvm-mingw,cmake] [-Force]
param(
    [string[]]$Only = @(),
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# "powershell -File" passes "-Only a,b" as ONE string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'toolchains\toolchains.json') | ConvertFrom-Json
$installRoot = [Environment]::ExpandEnvironmentVariables($manifest.installRoot)
$downloads = Join-Path $installRoot '_downloads'
New-Item -ItemType Directory -Force -Path $installRoot, $downloads | Out-Null

foreach ($tool in $manifest.tools) {
    if ($Only.Count -gt 0 -and $Only -notcontains $tool.id) { continue }
    # Optional tools (e.g. Ghidra, ~570 MB) are installed only when named explicitly with -Only.
    if ($Only.Count -eq 0 -and $tool.optional) { Write-Host "[skip] $($tool.id) $($tool.version) is optional (install with -Only $($tool.id))"; continue }
    $binDir = Join-Path $installRoot $tool.bin
    $probeExe = Join-Path $binDir $tool.probe[0]
    if ((Test-Path -LiteralPath $probeExe) -and -not $Force) {
        Write-Host "[ok]   $($tool.id) $($tool.version) already installed at $binDir"
        continue
    }

    $archive = Join-Path $downloads ([IO.Path]::GetFileName($tool.url))
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "[get]  $($tool.url)"
        & curl.exe -L --fail --silent --show-error -o "$archive.part" $tool.url
        if ($LASTEXITCODE -ne 0) { throw "download failed for $($tool.id) (curl exit $LASTEXITCODE)" }
        Move-Item -Force -LiteralPath "$archive.part" -Destination $archive
    }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    if ($hash -ne $tool.sha256.ToLowerInvariant()) {
        Remove-Item -Force -LiteralPath $archive
        throw "SHA-256 mismatch for $($tool.id): expected $($tool.sha256), got $hash (archive deleted)"
    }
    Write-Host "[hash] $($tool.id) sha256 verified"

    $dest = if ($tool.extractDir) { Join-Path $installRoot $tool.extractDir } else { $installRoot }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    # bsdtar (tar.exe, built into Windows 10+) extracts zips far faster than Expand-Archive. Called by full path: from a
    # Git Bash shell "tar.exe" resolves to GNU tar, which reads "C:" as a remote host and fails.
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $archive -C $dest
    if ($LASTEXITCODE -ne 0) { throw "extract failed for $($tool.id) (tar exit $LASTEXITCODE)" }

    if (-not (Test-Path -LiteralPath $probeExe)) { throw "$($tool.id): expected $probeExe after extraction" }
    if ($tool.probe.Count -gt 1) {
        $ver = & $probeExe $tool.probe[1..($tool.probe.Count - 1)] 2>&1 | Select-Object -First 1
        Write-Host "[ok]   $($tool.id): $ver"
    } else {
        Write-Host "[ok]   $($tool.id) $($tool.version): $probeExe present"
    }
}
