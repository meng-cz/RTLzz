param(
    [string]$BuildDir = "build",
    [string]$LlvmDir = "C:/Program Files/LLVM/lib/cmake/llvm",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Arch = "x64",
    [string]$Config = "Release",
    [string]$CustomerDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$LlvmBin = "C:\Program Files\LLVM\bin"

function Invoke-Checked {
    param(
        [string]$Name,
        [scriptblock]$Command
    )
    Write-Host "==> $Name"
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

try {
    if (Test-Path $LlvmBin) {
        $env:PATH = "$LlvmBin;$env:PATH"
    }
    $env:LLVM_DIR = $LlvmDir

    if (Test-Path $BuildPath) {
        Remove-Item -LiteralPath $BuildPath -Recurse -Force
    }

    Invoke-Checked "configure strict libclang" {
        $args = @(
            "-S", "$Root",
            "-B", "$BuildPath",
            "-G", "$Generator",
            "-DLLVM_DIR=$LlvmDir",
            "-DPREDICATE_EXPAND_REQUIRE_LIBCLANG=ON"
        )
        if ($Generator -match "Visual Studio" -and -not [string]::IsNullOrWhiteSpace($Arch)) {
            $args += @("-A", "$Arch")
        }
        if ($Generator -eq "Ninja") {
            $args += "-DCMAKE_BUILD_TYPE=$Config"
        }
        cmake @args
    }

    Invoke-Checked "build $Config" {
        cmake --build "$BuildPath" --config "$Config"
    }

    $ExeDir = if ($Generator -eq "Ninja") { $BuildPath } else { Join-Path $BuildPath $Config }
    if (!(Test-Path (Join-Path $ExeDir "predicate-expand.exe"))) {
        throw "Strict CI expected predicate-expand.exe to be built"
    }
    if (!(Test-Path (Join-Path $ExeDir "predicate-expand-e2e-tests.exe"))) {
        throw "Strict CI expected predicate-expand-e2e-tests.exe to be built"
    }

    Invoke-Checked "unsafe lowering check" {
        & (Join-Path $PSScriptRoot "check_no_unsafe_lowering.ps1")
    }

    Invoke-Checked "ctest unit+e2e" {
        ctest --test-dir "$BuildPath" -C "$Config" --output-on-failure
    }

    Invoke-Checked "smt solver validation" {
        & (Join-Path $PSScriptRoot "test_smt_solver.ps1") -BuildDir $BuildDir -RequireZ3
    }

    Invoke-Checked "exact customer files" {
        if (-not [string]::IsNullOrWhiteSpace($CustomerDir)) {
            & (Join-Path $PSScriptRoot "test_customer_files.ps1") -BuildDir $BuildDir -CustomerDir $CustomerDir
        } else {
            & (Join-Path $PSScriptRoot "test_customer_files.ps1") -BuildDir $BuildDir
        }
    }

    Invoke-Checked "generic fixture suite" {
        & (Join-Path $PSScriptRoot "test_real_vul_outputs.ps1") -BuildDir $BuildDir
    }

    Write-Host "Strict CI passed."
    exit 0
} catch {
    Write-Error "Strict CI failed: $($_.Exception.Message)"
    exit 1
}
