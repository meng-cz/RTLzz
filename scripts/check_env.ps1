param(
    [string]$BuildDir = "build",
    [string]$LLVMDir = "",
    [switch]$RequireExe
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$errors = 0

function Report([string]$Level, [string]$Name, [string]$Message) {
    Write-Host "[$Level] $Name - $Message"
    if ($Level -eq "ERROR") { $script:errors++ }
}

function Check-Command([string]$Name, [string]$Fix) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        Report "OK" $Name $cmd.Source
    } else {
        Report "ERROR" $Name "Not found. $Fix"
    }
}

function Find-LlvmRoot {
    if (-not [string]::IsNullOrWhiteSpace($LLVMDir)) {
        $p = $LLVMDir
    } elseif ($env:LLVM_DIR) {
        $p = $env:LLVM_DIR
    } else {
        $p = "C:\Program Files\LLVM"
    }
    if ($p -like "*\lib\cmake\llvm" -or $p -like "*/lib/cmake/llvm") {
        return [System.IO.Path]::GetFullPath((Join-Path $p "..\..\.."))
    }
    return [System.IO.Path]::GetFullPath($p)
}

Check-Command "cmake" "Install CMake and add it to PATH."

$cl = Get-Command "cl" -ErrorAction SilentlyContinue
if ($cl) {
    Report "OK" "MSVC cl" $cl.Source
} else {
    Report "WARN" "MSVC cl" "Not visible in this shell. Use Developer PowerShell/VS environment, or the Ninja fallback."
}

$LlvmRoot = Find-LlvmRoot
if (Test-Path -LiteralPath $LlvmRoot) {
    Report "OK" "LLVM root" $LlvmRoot
} else {
    Report "ERROR" "LLVM root" "Missing: $LlvmRoot. Install LLVM or pass -LLVMDir / set LLVM_DIR."
}

$IndexPath = Join-Path $LlvmRoot "include\clang-c\Index.h"
if (Test-Path -LiteralPath $IndexPath) {
    Report "OK" "clang-c/Index.h" $IndexPath
} else {
    Report "ERROR" "clang-c/Index.h" "Missing: $IndexPath. Install LLVM with libclang C API headers."
}

$LibPath = Join-Path $LlvmRoot "lib\libclang.lib"
if (Test-Path -LiteralPath $LibPath) {
    Report "OK" "libclang.lib" $LibPath
} else {
    Report "ERROR" "libclang.lib" "Missing: $LibPath. Install LLVM Windows package or set LLVM_DIR."
}

$DllPath = Join-Path $LlvmRoot "bin\libclang.dll"
if (Test-Path -LiteralPath $DllPath) {
    Report "OK" "libclang.dll" $DllPath
} else {
    Report "ERROR" "libclang.dll" "Missing: $DllPath. The CLI needs this DLL at runtime."
}

$Uint = Join-Path $Root "third_party\vulsim\vullib\uint.hpp"
if (Test-Path -LiteralPath $Uint) {
    Report "OK" "uint.hpp" $Uint
} else {
    Report "ERROR" "uint.hpp" "Missing third_party/vulsim/vullib/uint.hpp."
}

if (Test-Path -LiteralPath $BuildPath) {
    Report "OK" "build dir" $BuildPath
} else {
    Report "WARN" "build dir" "Missing $BuildPath. Run scripts/configure_vs2026.ps1 and scripts/build_release.ps1."
}

$ReleaseExe = Join-Path $BuildPath "Release\predicate-expand.exe"
$RootExe = Join-Path $BuildPath "predicate-expand.exe"
if (Test-Path -LiteralPath $ReleaseExe) {
    Report "OK" "predicate-expand.exe" $ReleaseExe
} elseif (Test-Path -LiteralPath $RootExe) {
    Report "OK" "predicate-expand.exe" $RootExe
} else {
    $msg = "Not found. Run scripts/build_release.ps1 -BuildDir $BuildDir. For unit-only mode, explicitly use -AllowUnitOnly."
    if ($RequireExe) {
        Report "ERROR" "predicate-expand.exe" $msg
    } else {
        Report "WARN" "predicate-expand.exe" "$msg Use -RequireExe to make this a required post-build check."
    }
}

if ($errors -gt 0) {
    Write-Host "[ERROR] Environment check failed: $errors required item(s) missing."
    exit 1
}

Write-Host "[OK] Environment check passed."
