param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$Runtime = "win-x64",
    [switch]$Archive,
    [switch]$SkipBuild,
    [string]$ArchiveName = "RTL100.zip",
    [string]$ArchivePath = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$DistRoot = Join-Path $Root "dist"
$DistPath = Join-Path $DistRoot "RTLzz-GUI"
$GuiProject = Join-Path $Root "gui\RTLzz.Gui\RTLzz.Gui.csproj"

function Resolve-PredicateExe {
    $candidates = @(
        (Join-Path $BuildPath "$Configuration\predicate-expand.exe"),
        (Join-Path $BuildPath "predicate-expand.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "predicate-expand.exe not found under build directory: $BuildPath"
}

function Resolve-LibClang([string]$PredicateExe) {
    $candidates = @(
        (Join-Path (Split-Path -Parent $PredicateExe) "libclang.dll"),
        "C:\Program Files\LLVM\bin\libclang.dll"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "libclang.dll not found. Install LLVM or build predicate-expand with libclang copied next to the exe."
}

function Find-VcRedistDir {
    $roots = @(
        "C:\Program Files\Microsoft Visual Studio",
        "C:\Program Files (x86)\Microsoft Visual Studio"
    )
    $dirs = @()
    foreach ($root in $roots) {
        if (Test-Path -LiteralPath $root) {
            $dirs += Get-ChildItem -LiteralPath $root -Recurse -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\VC\\Redist\\MSVC\\[^\\]+\\x64\\Microsoft\.VC.*\.CRT$' }
        }
    }
    $dirs | Sort-Object FullName -Descending | Select-Object -First 1
}

function Copy-VcRuntime([string]$RuntimeDir) {
    $copied = New-Object System.Collections.Generic.List[string]
    $redist = Find-VcRedistDir
    if ($null -eq $redist) {
        Write-Warning "VC++ Redistributable DLL source not found. Customer machine may need Microsoft Visual C++ Runtime installed."
        return $copied
    }
    foreach ($pattern in @("vcruntime*.dll", "msvcp*.dll", "concrt*.dll")) {
        foreach ($dll in Get-ChildItem -LiteralPath $redist.FullName -Filter $pattern -File -ErrorAction SilentlyContinue) {
            Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $RuntimeDir $dll.Name) -Force
            $copied.Add($dll.Name) | Out-Null
        }
    }
    return $copied
}

function Remove-ReleaseNoise([string]$Path) {
    $forbidden = Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Extension -in @(".pdb", ".ilk", ".obj", ".log") -or
            $_.Name -ieq "settings.json" -or
            $_.Name -match '^(README|LICENSE|VERSION)(\.|$)' -or
            $_.Name -ieq "release-manifest.json"
        }
    foreach ($item in @($forbidden)) {
        if ($null -ne $item) {
            Remove-Item -LiteralPath $item.FullName -Force
        }
    }
}

function Assert-RequiredFiles([string]$Path) {
    $required = @(
        "RTLzz-GUI.exe",
        "predicate-expand.exe",
        "runtime\libclang.dll",
        "third_party\vulsim\vullib\uint.hpp",
        "third_party\vulsim\vullib\common.h"
    )
    foreach ($rel in $required) {
        $full = Join-Path $Path $rel
        if (-not (Test-Path -LiteralPath $full)) {
            throw "required release file missing: $rel"
        }
    }
}

if ($ArchiveName -notmatch '\.zip$') {
    $ArchiveName = "$ArchiveName.zip"
}
if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
    $ArchivePath = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)) $ArchiveName
}
$ArchivePath = if ([System.IO.Path]::IsPathRooted($ArchivePath)) { $ArchivePath } else { Join-Path $Root $ArchivePath }

if (-not $SkipBuild) {
    Write-Host "Running C++ build and tests..."
    & (Join-Path $PSScriptRoot "test_all.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "C++ tests failed with exit code $LASTEXITCODE" }

    Write-Host "Running GUI tests..."
    & (Join-Path $PSScriptRoot "test_gui.ps1") -BuildDir $BuildDir -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw "GUI tests failed with exit code $LASTEXITCODE" }
}

$PredicateExe = Resolve-PredicateExe
$LibClang = Resolve-LibClang $PredicateExe

if (Test-Path -LiteralPath $DistPath) {
    Remove-Item -LiteralPath $DistPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $DistPath | Out-Null

Write-Host "Publishing GUI self-contained $Runtime..."
dotnet publish $GuiProject `
    -c $Configuration `
    -r $Runtime `
    --self-contained true `
    -o $DistPath `
    /p:PublishSingleFile=false `
    /p:DebugType=none `
    /p:DebugSymbols=false `
    /p:Deterministic=true `
    /p:ContinuousIntegrationBuild=true
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed with exit code $LASTEXITCODE" }

Copy-Item -LiteralPath $PredicateExe -Destination (Join-Path $DistPath "predicate-expand.exe") -Force

$RuntimeDir = Join-Path $DistPath "runtime"
New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
Copy-Item -LiteralPath $LibClang -Destination (Join-Path $RuntimeDir "libclang.dll") -Force
$VcDlls = Copy-VcRuntime $RuntimeDir

$ThirdPartySrc = Join-Path $Root "third_party\vulsim\vullib"
$ThirdPartyDst = Join-Path $DistPath "third_party\vulsim\vullib"
New-Item -ItemType Directory -Force -Path $ThirdPartyDst | Out-Null
foreach ($header in @("uint.hpp", "common.h")) {
    $src = Join-Path $ThirdPartySrc $header
    if (-not (Test-Path -LiteralPath $src)) { throw "required header missing: $src" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $ThirdPartyDst $header) -Force
}

Remove-ReleaseNoise $DistPath
Assert-RequiredFiles $DistPath

Write-Host "Running release privacy scan on directory..."
& (Join-Path $PSScriptRoot "check_release_privacy.ps1") -ReleaseDir $DistPath
if ($LASTEXITCODE -ne 0) { throw "release privacy scan failed with exit code $LASTEXITCODE" }

Write-Host "Running portable release test on directory..."
& (Join-Path $PSScriptRoot "test_portable_release.ps1") -ReleaseDir $DistPath
if ($LASTEXITCODE -ne 0) { throw "portable release directory test failed with exit code $LASTEXITCODE" }

if ($Archive) {
    $archiveDir = Split-Path -Parent $ArchivePath
    if (-not [string]::IsNullOrWhiteSpace($archiveDir)) {
        New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null
    }
    $tmpZip = Join-Path ([System.IO.Path]::GetTempPath()) ("rtlzz_customer_" + [Guid]::NewGuid().ToString("N") + ".zip")
    if (Test-Path -LiteralPath $tmpZip) { Remove-Item -LiteralPath $tmpZip -Force }

    Write-Host "Creating archive: $ArchivePath"
    Compress-Archive -LiteralPath $DistPath -DestinationPath $tmpZip -CompressionLevel Optimal

    & (Join-Path $PSScriptRoot "check_release_privacy.ps1") -ZipPath $tmpZip
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $tmpZip -Force -ErrorAction SilentlyContinue
        throw "archive privacy scan failed with exit code $LASTEXITCODE"
    }

    & (Join-Path $PSScriptRoot "test_portable_release.ps1") -ZipPath $tmpZip
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $tmpZip -Force -ErrorAction SilentlyContinue
        throw "portable archive test failed with exit code $LASTEXITCODE"
    }

    Move-Item -LiteralPath $tmpZip -Destination $ArchivePath -Force
    $sha = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
    $size = (Get-Item -LiteralPath $ArchivePath).Length
    Write-Host "Archive: $ArchivePath"
    Write-Host "ArchiveSize: $size"
    Write-Host "ArchiveSHA256: $sha"
}

Write-Host "Customer release directory: $DistPath"
Write-Host "SelfContained: true"
Write-Host "RequiresDotNetRuntime: false"
if ($VcDlls.Count -gt 0) {
    Write-Host ("RequiresVCRuntime: false; packaged DLLs: " + (($VcDlls | Sort-Object -Unique) -join ", "))
} else {
    Write-Host "RequiresVCRuntime: maybe; VC++ runtime DLLs were not found for packaging"
}
