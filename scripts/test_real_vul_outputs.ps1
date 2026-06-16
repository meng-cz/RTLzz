param(
    [string]$OutputDir = "",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
    $OutputDir = Join-Path $BuildPath "generic_fixtures"
}
$IncludeDir = Join-Path $Root "third_party\vulsim\vullib"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Fail([string]$Message) { throw $Message }

function RunFixture([System.IO.FileInfo]$Fixture, [string]$Format) {
    $input = $Fixture.FullName
    if (-not (Test-Path -LiteralPath $input)) { Fail "Missing fixture: $input" }
    $Name = $Fixture.BaseName -replace '\.logic$', ''
    $output = Join-Path $OutputDir "$Name.$Format"
    & (Join-Path $PSScriptRoot "run_real_case.ps1") `
        -Input $input `
        -Top hls_main `
        -Format $Format `
        -Output $output `
        -BuildDir $BuildDir `
        -ClangArg @("-I$IncludeDir", "-std=c++20")
    if ($LASTEXITCODE -ne 0) { Fail "Fixture $Name $Format failed with exit code $LASTEXITCODE" }
    return $output
}

$FixtureDir = Join-Path $Root "tests\fixtures"
$fixtures = Get-ChildItem -LiteralPath $FixtureDir -File -Filter "*.logic.cpp" |
    Sort-Object -Property Name
if ($fixtures.Count -eq 0) {
    Fail "No positive fixtures found under $FixtureDir"
}

$jsonFiles = @()
$textFiles = @()
foreach ($fixture in $fixtures) {
    $fixtureName = $fixture.BaseName -replace '\.logic$', ''
    Write-Host "Running generic fixture $fixtureName..."
    $textFiles += RunFixture $fixture "text"
    $jsonFiles += RunFixture $fixture "json"
    $jsonFiles += RunFixture $fixture "stable_json"
}

& (Join-Path $PSScriptRoot "verify_real_vul_outputs.ps1") -JsonPath $jsonFiles -TextPath $textFiles
if ($LASTEXITCODE -ne 0) { Fail "Generic fixture verification failed with exit code $LASTEXITCODE" }

Write-Host "Generic fixture integration tests passed."
