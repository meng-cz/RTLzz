param(
    [string]$BuildDir = "build",
    [string]$LlvmDir = "C:/Program Files/LLVM/lib/cmake/llvm",
    [switch]$AllowUnitOnly
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$LlvmBin = "C:\Program Files\LLVM\bin"
$Generator = "Visual Studio 18 2026"
$Platform = "x64"
$NinjaGenerator = "Ninja"
$ClangCxx = Join-Path $LlvmBin "clang++.exe"
$LlvmRc = Join-Path $LlvmBin "llvm-rc.exe"
$ClangCxxCMake = $ClangCxx.Replace('\', '/')
$LlvmRcCMake = $LlvmRc.Replace('\', '/')

function Reset-BuildDir {
    param([string]$Path)
    $ResolvedBuild = [System.IO.Path]::GetFullPath($Path)
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    if (!$ResolvedBuild.StartsWith($ResolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to recreate build directory outside project root: $ResolvedBuild"
    }
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

try {
    $UseFallbackOnly = $false
    if (Test-Path $LlvmBin) {
        $env:PATH = "$LlvmBin;$env:PATH"
    }
    $env:LLVM_DIR = $LlvmDir
    $RequireLibclang = if ($AllowUnitOnly) { "OFF" } else { "ON" }

    if (!(Test-Path $BuildPath)) {
        New-Item -ItemType Directory -Path $BuildPath | Out-Null
    }

    $Cache = Join-Path $BuildPath "CMakeCache.txt"
    if (Test-Path $Cache) {
        $CurrentGenerator = Select-String -Path $Cache -Pattern "^CMAKE_GENERATOR:INTERNAL=(.*)$" | ForEach-Object { $_.Matches[0].Groups[1].Value }
        $CurrentPlatform = Select-String -Path $Cache -Pattern "^CMAKE_GENERATOR_PLATFORM:INTERNAL=(.*)$" | ForEach-Object { $_.Matches[0].Groups[1].Value }
        $CurrentSource = Select-String -Path $Cache -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$" | ForEach-Object { $_.Matches[0].Groups[1].Value }
        $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
        $SourceMismatch = $CurrentSource -and ([System.IO.Path]::GetFullPath($CurrentSource) -ne $ResolvedRoot)
        if (!$SourceMismatch -and $CurrentGenerator -eq $NinjaGenerator) {
            $UseFallbackOnly = $true
        }
        if ($SourceMismatch -or ($CurrentGenerator -and $CurrentGenerator -ne $Generator) -or ($CurrentPlatform -and $CurrentPlatform -ne $Platform)) {
            if (!$UseFallbackOnly) {
                Write-Host "Existing build directory uses generator '$CurrentGenerator' platform '$CurrentPlatform'. Recreating build directory for '$Generator' '$Platform'."
                Reset-BuildDir -Path $BuildPath
            }
        }
    }

    if (!$UseFallbackOnly) {
        cmake -S "$Root" -B "$BuildPath" -G "$Generator" -A $Platform -DLLVM_DIR="$LlvmDir" -DPREDICATE_EXPAND_REQUIRE_LIBCLANG="$RequireLibclang"
        if ($LASTEXITCODE -eq 0) {
            exit 0
        }
        Write-Warning "VS2026 configure failed with exit code $LASTEXITCODE. Falling back to LLVM clang++ + Ninja."
        Reset-BuildDir -Path $BuildPath
    } else {
        Write-Host "Existing build directory uses LLVM clang++ + Ninja fallback. Refreshing fallback configuration."
    }
    if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw "CMake configure failed with VS2026 and Ninja is not available"
    }
    if (!(Test-Path $ClangCxx)) {
        throw "CMake configure failed with VS2026 and clang++ was not found at $ClangCxx"
    }
    $FallbackArgs = @(
        "-S", "$Root",
        "-B", "$BuildPath",
        "-G", "$NinjaGenerator",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=$ClangCxxCMake",
        "-DLLVM_DIR=$LlvmDir",
        "-DPREDICATE_EXPAND_REQUIRE_LIBCLANG=$RequireLibclang"
    )
    if (Test-Path $LlvmRc) {
        $FallbackArgs += "-DCMAKE_RC_COMPILER=$LlvmRcCMake"
    }
    cmake @FallbackArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake fallback configure failed with exit code $LASTEXITCODE"
    }
} catch {
    Write-Error "Configure VS2026 failed: $($_.Exception.Message)"
    exit 1
}
