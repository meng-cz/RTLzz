param(
    [string]$BuildDir = "build",
    [string]$CustomerDir = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$OutDir = Join-Path $BuildPath "customer"
$IncludeDir = Join-Path $Root "third_party\vulsim\vullib"
$ExplicitCustomerDir = -not [string]::IsNullOrWhiteSpace($CustomerDir)
if ([string]::IsNullOrWhiteSpace($CustomerDir)) {
    $CustomerDir = Join-Path $Root "tests\customer"
} elseif (-not [System.IO.Path]::IsPathRooted($CustomerDir)) {
    $CustomerDir = Join-Path $Root $CustomerDir
}
$TopInput = Join-Path $CustomerDir "top.logic.cpp"
$ConsInput = Join-Path $CustomerDir "cons.logic.cpp"

function Fail([string]$Message) {
    throw $Message
}

if (-not (Test-Path -LiteralPath $TopInput) -or -not (Test-Path -LiteralPath $ConsInput)) {
    if (-not $ExplicitCustomerDir) {
        Write-Host "[SKIP] customer cases not found. Pass -CustomerDir to enable them."
        exit 0
    }
    if (-not (Test-Path -LiteralPath $TopInput)) { Fail "Missing exact customer file: $TopInput" }
    if (-not (Test-Path -LiteralPath $ConsInput)) { Fail "Missing exact customer file: $ConsInput" }
}
if (-not (Test-Path -LiteralPath $IncludeDir)) {
    Fail "Missing VUL include directory: $IncludeDir"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function RunCustomerCase(
    [string]$Name,
    [string]$Input,
    [string]$Top,
    [string]$Format
) {
    $output = Join-Path $OutDir "$Name.$Format"
    & (Join-Path $PSScriptRoot "run_real_case.ps1") `
        -Input $Input `
        -Top $Top `
        -Format $Format `
        -Output $output `
        -BuildDir $BuildDir `
        -ClangArg @("-I$IncludeDir", "-std=c++20")
    if ($LASTEXITCODE -ne 0) {
        Fail "Customer case $Name $Format failed with exit code $LASTEXITCODE"
    }
    return $output
}

Write-Host "Running exact customer top.logic.cpp..."
$jsonFiles = @()
$textFiles = @()
$textFiles += RunCustomerCase "top.logic" $TopInput "LogicSubModule_AES1_top" "text"
$jsonFiles += RunCustomerCase "top.logic" $TopInput "LogicSubModule_AES1_top" "json"
$jsonFiles += RunCustomerCase "top.logic" $TopInput "LogicSubModule_AES1_top" "stable_json"
[void](RunCustomerCase "top.logic" $TopInput "LogicSubModule_AES1_top" "smt")

Write-Host "Running exact customer cons.logic.cpp..."
$textFiles += RunCustomerCase "cons.logic" $ConsInput "LogicSubModule_Consumer_top_cons" "text"
$jsonFiles += RunCustomerCase "cons.logic" $ConsInput "LogicSubModule_Consumer_top_cons" "json"
$jsonFiles += RunCustomerCase "cons.logic" $ConsInput "LogicSubModule_Consumer_top_cons" "stable_json"
[void](RunCustomerCase "cons.logic" $ConsInput "LogicSubModule_Consumer_top_cons" "smt")

& (Join-Path $PSScriptRoot "verify_real_vul_outputs.ps1") -JsonPath $jsonFiles -TextPath $textFiles
if ($LASTEXITCODE -ne 0) {
    Fail "Customer output verification failed with exit code $LASTEXITCODE"
}

Write-Host "Exact customer files passed."
