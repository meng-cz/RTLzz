param(
    [string]$BuildDir = "build",
    [switch]$SkipRealVul,
    [string]$CustomerDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$LlvmBin = "C:\Program Files\LLVM\bin"

try {
    & (Join-Path $PSScriptRoot "build_release.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if (Test-Path $LlvmBin) {
        $env:PATH = "$LlvmBin;$env:PATH"
    }

    & (Join-Path $PSScriptRoot "check_no_unsafe_lowering.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Unsafe lowering check failed with exit code $LASTEXITCODE"
    }

    ctest --test-dir "$BuildPath" -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with exit code $LASTEXITCODE"
    }

    & (Join-Path $PSScriptRoot "test_smt_solver.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "SMT solver validation failed with exit code $LASTEXITCODE"
    }

    if (-not $SkipRealVul) {
        & (Join-Path $PSScriptRoot "test_real_vul_outputs.ps1") -BuildDir $BuildDir
        if ($LASTEXITCODE -ne 0) {
            throw "Generic fixture integration test failed with exit code $LASTEXITCODE"
        }

        if (-not [string]::IsNullOrWhiteSpace($CustomerDir)) {
            & (Join-Path $PSScriptRoot "test_customer_files.ps1") -BuildDir $BuildDir -CustomerDir $CustomerDir
        } else {
            & (Join-Path $PSScriptRoot "test_customer_files.ps1") -BuildDir $BuildDir
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Exact customer regression failed with exit code $LASTEXITCODE"
        }
    }
} catch {
    Write-Error "Test All failed: $($_.Exception.Message)"
    exit 1
}
