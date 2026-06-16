param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$DistDir = "dist\RTLzz-GUI"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$DistPath = if ([System.IO.Path]::IsPathRooted($DistDir)) { $DistDir } else { Join-Path $Root $DistDir }
$GuiProject = Join-Path $Root "gui\RTLzz.Gui\RTLzz.Gui.csproj"

Write-Host "Building predicate-expand Release..."
& (Join-Path $PSScriptRoot "build_release.ps1") -BuildDir $BuildDir
if ($LASTEXITCODE -ne 0) { throw "predicate-expand build failed with exit code $LASTEXITCODE" }

$PredicateExe = Join-Path $BuildPath "$Configuration\predicate-expand.exe"
if (-not (Test-Path -LiteralPath $PredicateExe)) {
    $PredicateExe = Join-Path $BuildPath "predicate-expand.exe"
}
if (-not (Test-Path -LiteralPath $PredicateExe)) {
    throw "predicate-expand.exe not found after build: $PredicateExe"
}

if (Test-Path -LiteralPath $DistPath) {
    Remove-Item -LiteralPath $DistPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $DistPath | Out-Null

Write-Host "Publishing GUI..."
dotnet publish $GuiProject -c $Configuration -o $DistPath --self-contained false
if ($LASTEXITCODE -ne 0) { throw "GUI publish failed with exit code $LASTEXITCODE" }

Copy-Item -LiteralPath $PredicateExe -Destination (Join-Path $DistPath "predicate-expand.exe") -Force
$LibClang = Join-Path (Split-Path -Parent $PredicateExe) "libclang.dll"
if (Test-Path -LiteralPath $LibClang) {
    Copy-Item -LiteralPath $LibClang -Destination (Join-Path $DistPath "libclang.dll") -Force
}

$ThirdPartySrc = Join-Path $Root "third_party\vulsim\vullib"
$ThirdPartyDst = Join-Path $DistPath "third_party\vulsim\vullib"
if (Test-Path -LiteralPath $ThirdPartySrc) {
    New-Item -ItemType Directory -Force -Path $ThirdPartyDst | Out-Null
    Copy-Item -Path (Join-Path $ThirdPartySrc "*") -Destination $ThirdPartyDst -Recurse -Force
}

$GeneratedNoise = Get-ChildItem -LiteralPath $DistPath -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Extension -in @(".pdb", ".log") -or $_.Name -ieq "settings.json"
    }
foreach ($Item in @($GeneratedNoise)) {
    if ($null -ne $Item) {
        try {
            if (-not $Item.PSIsContainer -and [System.IO.File]::Exists($Item.FullName)) {
                [System.IO.File]::Delete($Item.FullName)
            }
        } catch {
            Write-Warning "Failed to remove generated file $($Item.FullName): $($_.Exception.Message)"
        }
    }
}

Write-Host "GUI dist created: $DistPath"
