param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [switch]$SkipRunnerSmoke
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$GuiProject = Join-Path $Root "gui\RTLzz.Gui\RTLzz.Gui.csproj"
$TestProject = Join-Path $Root "gui\RTLzz.Gui.Tests\RTLzz.Gui.Tests.csproj"
$IncludeDir = Join-Path $Root "third_party\vulsim\vullib"
$Exe = Join-Path $BuildPath "$Configuration\predicate-expand.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    $Exe = Join-Path $BuildPath "predicate-expand.exe"
}

Write-Host "Building GUI..."
dotnet build $GuiProject -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "GUI build failed with exit code $LASTEXITCODE" }

Write-Host "Building GUI service tests..."
dotnet build $TestProject -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "GUI test build failed with exit code $LASTEXITCODE" }

Write-Host "Running GUI service tests..."
dotnet run --project $TestProject -c $Configuration --no-build
if ($LASTEXITCODE -ne 0) { throw "GUI service tests failed with exit code $LASTEXITCODE" }

if (-not $SkipRunnerSmoke) {
    if (-not (Test-Path -LiteralPath $Exe)) {
        throw "predicate-expand.exe not found for runner smoke: $Exe"
    }
    $SmokeOut = Join-Path $BuildPath "gui_runner_smoke"
    New-Item -ItemType Directory -Force -Path $SmokeOut | Out-Null
    Write-Host "Running GUI runner smoke..."
    dotnet run --project $TestProject -c $Configuration --no-build -- --runner-smoke $Exe $IncludeDir $SmokeOut
    if ($LASTEXITCODE -ne 0) { throw "GUI runner smoke failed with exit code $LASTEXITCODE" }
}

Write-Host "GUI tests passed."
