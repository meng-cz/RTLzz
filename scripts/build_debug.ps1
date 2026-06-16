param(
    [string]$BuildDir = "build",
    [switch]$AllowUnitOnly
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$LlvmBin = "C:\Program Files\LLVM\bin"

try {
    if ($AllowUnitOnly) {
        & (Join-Path $PSScriptRoot "configure_vs2026.ps1") -BuildDir $BuildDir -AllowUnitOnly
    } else {
        & (Join-Path $PSScriptRoot "configure_vs2026.ps1") -BuildDir $BuildDir
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if (Test-Path $LlvmBin) {
        $env:PATH = "$LlvmBin;$env:PATH"
    }

    cmake --build "$BuildPath" --config Debug
    if ($LASTEXITCODE -ne 0) {
        throw "CMake Debug build failed with exit code $LASTEXITCODE"
    }
    if (-not $AllowUnitOnly) {
        $DebugExe = Join-Path $BuildPath "Debug\predicate-expand.exe"
        $RootExe = Join-Path $BuildPath "predicate-expand.exe"
        if (!(Test-Path -LiteralPath $DebugExe) -and !(Test-Path -LiteralPath $RootExe)) {
            throw "Debug build completed but predicate-expand.exe was not produced. Check libclang configuration or rerun with -AllowUnitOnly for unit-only mode."
        }
    }
} catch {
    Write-Error "Build Debug failed: $($_.Exception.Message)"
    exit 1
}
