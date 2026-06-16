param(
    [Parameter(Mandatory = $true)]
    [Alias("Input")]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Top,

    [ValidateSet("text", "json", "stable_json", "smt")]
    [string]$Format = "text",

    [string]$Output,

    [int]$UnrollLimit = 4096,

    [string[]]$ClangArg = @(),

    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$ReleaseExe = Join-Path $BuildPath "Release\predicate-expand.exe"
$DebugExe = Join-Path $BuildPath "Debug\predicate-expand.exe"
$RootExe = Join-Path $BuildPath "predicate-expand.exe"
$LlvmBin = "C:\Program Files\LLVM\bin"

try {
    if (!(Test-Path $ReleaseExe) -and !(Test-Path $DebugExe) -and !(Test-Path $RootExe)) {
        & (Join-Path $PSScriptRoot "build_release.ps1") -BuildDir $BuildDir
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    if (Test-Path $ReleaseExe) {
        $Exe = $ReleaseExe
    } elseif (Test-Path $DebugExe) {
        $Exe = $DebugExe
    } elseif (Test-Path $RootExe) {
        $Exe = $RootExe
    } else {
        throw "predicate-expand.exe was not found after build"
    }

    if (Test-Path $LlvmBin) {
        $env:PATH = "$LlvmBin;$env:PATH"
    }

    if (!$env:PREDICATE_EXPAND_CLANG_ARGS) {
        $env:PREDICATE_EXPAND_CLANG_ARGS = "-I."
    }

    $CommandArgs = @(
        $Source,
        "--top", $Top,
        "--format", $Format,
        "--unroll-limit", "$UnrollLimit"
    )

    foreach ($Arg in $ClangArg) {
        $CommandArgs += @("--clang-arg", $Arg)
    }

    if ($Output) {
        $CommandArgs += @("-o", $Output)
    }

    Push-Location $Root
    try {
        & $Exe @CommandArgs
        if ($LASTEXITCODE -ne 0) {
            throw "predicate-expand failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
