param(
    [string]$ReleaseDir = "",
    [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"

function Resolve-PackageRoot([string]$Path) {
    $root = (Resolve-Path -LiteralPath $Path).Path
    $nested = Join-Path $root "RTLzz-GUI"
    if (Test-Path -LiteralPath $nested) {
        return (Resolve-Path -LiteralPath $nested).Path
    }
    return $root
}

function Invoke-PredicateSmoke([string]$PackageRoot, [string]$WorkRoot) {
    $predicate = Join-Path $PackageRoot "predicate-expand.exe"
    $include = Join-Path $PackageRoot "third_party\vulsim\vullib"
    $runtime = Join-Path $PackageRoot "runtime"
    if (-not (Test-Path -LiteralPath $predicate)) { throw "predicate-expand.exe not found in portable package" }
    if (-not (Test-Path -LiteralPath (Join-Path $include "uint.hpp"))) { throw "uint.hpp not found in portable package" }

    $src = Join-Path $WorkRoot "portable_smoke.cpp"
    $textOut = Join-Path $WorkRoot "portable_smoke.txt"
    $jsonOut = Join-Path $WorkRoot "portable_smoke.json"
    @"
#include <uint.hpp>
void hls_main(Int<8> a, Int<8>& out) {
    out = a + Int<8>(1);
}
"@ | Set-Content -LiteralPath $src -Encoding UTF8

    $oldPath = $env:PATH
    try {
        $env:PATH = "$runtime;$PackageRoot;$oldPath"
        & $predicate $src --top hls_main --format text --unroll-limit 4096 --clang-arg "-std=c++20" --clang-arg "-I$include" -o $textOut
        if ($LASTEXITCODE -ne 0) { throw "portable text generation failed with exit code $LASTEXITCODE" }
        & $predicate $src --top hls_main --format json --unroll-limit 4096 --clang-arg "-std=c++20" --clang-arg "-I$include" -o $jsonOut
        if ($LASTEXITCODE -ne 0) { throw "portable json generation failed with exit code $LASTEXITCODE" }
    } finally {
        $env:PATH = $oldPath
    }

    if (-not (Test-Path -LiteralPath $textOut) -or (Get-Item -LiteralPath $textOut).Length -le 0) {
        throw "portable text output missing or empty"
    }
    if (-not (Test-Path -LiteralPath $jsonOut) -or (Get-Item -LiteralPath $jsonOut).Length -le 0) {
        throw "portable json output missing or empty"
    }
    Get-Content -LiteralPath $jsonOut -Raw | ConvertFrom-Json | Out-Null
}

function Invoke-GuiSmoke([string]$PackageRoot, [string]$WorkRoot) {
    $gui = Join-Path $PackageRoot "RTLzz-GUI.exe"
    if (-not (Test-Path -LiteralPath $gui)) { throw "RTLzz-GUI.exe not found in portable package" }

    $localAppData = Join-Path $WorkRoot "localappdata"
    New-Item -ItemType Directory -Force -Path $localAppData | Out-Null
    $psi = [System.Diagnostics.ProcessStartInfo]::new($gui)
    $psi.WorkingDirectory = $PackageRoot
    $psi.UseShellExecute = $false
    $psi.Environment["LOCALAPPDATA"] = $localAppData
    $psi.Environment["PATH"] = (Join-Path $PackageRoot "runtime") + [System.IO.Path]::PathSeparator + $PackageRoot + [System.IO.Path]::PathSeparator + $psi.Environment["PATH"]
    $process = [System.Diagnostics.Process]::Start($psi)
    if ($null -eq $process) { throw "failed to start GUI from portable package" }
    try {
        Start-Sleep -Seconds 5
        if ($process.HasExited) {
            throw "GUI exited immediately with code $($process.ExitCode)"
        }
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(3000)) {
            $process.Kill($true)
            $process.WaitForExit()
        }
    } finally {
        if (-not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($ReleaseDir) -and [string]::IsNullOrWhiteSpace($ZipPath)) {
    throw "Pass -ReleaseDir or -ZipPath"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("rtlzz_portable_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    if (-not [string]::IsNullOrWhiteSpace($ZipPath)) {
        $extract = Join-Path $temp "extract"
        New-Item -ItemType Directory -Force -Path $extract | Out-Null
        Expand-Archive -LiteralPath $ZipPath -DestinationPath $extract -Force
        $packageRoot = Resolve-PackageRoot $extract
    } else {
        $copy = Join-Path $temp "RTLzz-GUI"
        Copy-Item -LiteralPath $ReleaseDir -Destination $copy -Recurse -Force
        $packageRoot = Resolve-PackageRoot $copy
    }

    Invoke-PredicateSmoke $packageRoot $temp
    Invoke-GuiSmoke $packageRoot $temp
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Portable release test passed."
