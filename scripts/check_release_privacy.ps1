param(
    [string]$ReleaseDir = "",
    [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Issues = New-Object System.Collections.Generic.List[string]

function Add-Issue([string]$Message) {
    $Issues.Add($Message) | Out-Null
}

function Get-RelativePathCompat([string]$Base, [string]$Path) {
    $baseFull = [System.IO.Path]::GetFullPath($Base)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    $baseUri = [Uri]::new($baseFull)
    $pathUri = [Uri]::new($pathFull)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

function Test-ForbiddenName([System.IO.FileSystemInfo]$Item, [string]$Base) {
    $rel = Get-RelativePathCompat $Base $Item.FullName
    $name = $Item.Name
    $parts = $rel -split '[\\/]'
    $lowerName = $name.ToLowerInvariant()
    $lowerRel = $rel.ToLowerInvariant()

    $forbiddenExact = @(
        "readme", "readme.md", "readme-client.md", "readme-gui.md",
        "license", "license.txt", "version.txt", "release-manifest.json",
        "settings.json"
    )
    if ($forbiddenExact -contains $lowerName) {
        Add-Issue "forbidden file name: $rel"
    }

    foreach ($part in $parts) {
        $p = $part.ToLowerInvariant()
        if ($p -in @(".git", ".vs", ".vscode", "obj", "build", "cmakefiles", "tests", "scripts", "gui", "src")) {
            Add-Issue "forbidden directory in package: $rel"
            break
        }
    }

    if ($lowerName -eq "cmakecache.txt" -or
        $lowerName.EndsWith(".pdb") -or
        $lowerName.EndsWith(".ilk") -or
        $lowerName.EndsWith(".obj") -or
        $lowerName.EndsWith(".log") -or
        $lowerName.EndsWith(".cpp") -or
        $lowerName.EndsWith(".cs") -or
        $lowerName.EndsWith(".vcxproj") -or
        $lowerName.EndsWith(".csproj")) {
        Add-Issue "forbidden development artifact: $rel"
    }

    if ($lowerRel.Contains("desktop\rtlzz") -or $lowerRel.Contains("desktop/rtlzz")) {
        Add-Issue "developer path fragment in package path: $rel"
    }
}

function Get-AsciiStrings([byte[]]$Bytes) {
    $builder = New-Object System.Text.StringBuilder
    foreach ($b in $Bytes) {
        if ($b -ge 32 -and $b -le 126) {
            [void]$builder.Append([char]$b)
        } else {
            if ($builder.Length -ge 6) { $builder.ToString() }
            [void]$builder.Clear()
        }
    }
    if ($builder.Length -ge 6) { $builder.ToString() }
}

function Get-Utf16Strings([byte[]]$Bytes) {
    $builder = New-Object System.Text.StringBuilder
    for ($i = 0; $i + 1 -lt $Bytes.Length; $i += 2) {
        $code = $Bytes[$i] -bor ($Bytes[$i + 1] -shl 8)
        if ($code -ge 32 -and $code -le 126) {
            [void]$builder.Append([char]$code)
        } else {
            if ($builder.Length -ge 6) { $builder.ToString() }
            [void]$builder.Clear()
        }
    }
    if ($builder.Length -ge 6) { $builder.ToString() }
}

function Test-Content([System.IO.FileInfo]$File, [string]$Base) {
    $rel = Get-RelativePathCompat $Base $File.FullName
    $terms = New-Object System.Collections.Generic.List[string]
    $terms.Add("C:\Users\") | Out-Null
    $terms.Add("Desktop\RTLzz") | Out-Null
    $terms.Add($Root) | Out-Null
    $terms.Add(".codex") | Out-Null
    if (-not [string]::IsNullOrWhiteSpace($env:USERNAME) -and
        $env:USERNAME -notin @("Administrator", "Admin") -and
        $env:USERNAME.Length -ge 6) {
        $terms.Add($env:USERNAME) | Out-Null
    }
    if (-not [string]::IsNullOrWhiteSpace($env:COMPUTERNAME) -and $env:COMPUTERNAME.Length -ge 6) {
        $terms.Add($env:COMPUTERNAME) | Out-Null
    }

    $ext = $File.Extension.ToLowerInvariant()
    if ($ext -in @(".exe", ".dll")) {
        $bytes = [System.IO.File]::ReadAllBytes($File.FullName)
        $strings = @()
        $strings += @(Get-AsciiStrings $bytes)
        $strings += @(Get-Utf16Strings $bytes)
        foreach ($s in $strings) {
            foreach ($term in $terms) {
                if (-not [string]::IsNullOrWhiteSpace($term) -and $s.IndexOf($term, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    Add-Issue "private string '$term' in binary: $rel"
                    return
                }
            }
        }
    } elseif ($ext -in @(".json", ".config", ".txt", ".xml", ".cmd", ".ps1", ".bat")) {
        $text = [System.IO.File]::ReadAllText($File.FullName)
        foreach ($term in $terms) {
            if (-not [string]::IsNullOrWhiteSpace($term) -and $text.IndexOf($term, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                Add-Issue "private string '$term' in text file: $rel"
            }
        }
    }
}

function Test-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Release path does not exist: $Path"
    }
    $base = (Resolve-Path -LiteralPath $Path).Path
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        Test-ForbiddenName $item $base
        if (-not $item.PSIsContainer) {
            Test-Content ([System.IO.FileInfo]$item.FullName) $base
        }
    }
}

if (-not [string]::IsNullOrWhiteSpace($ReleaseDir)) {
    Write-Host "Scanning release directory: $ReleaseDir"
    Test-Tree $ReleaseDir
}

if (-not [string]::IsNullOrWhiteSpace($ZipPath)) {
    if (-not (Test-Path -LiteralPath $ZipPath)) {
        throw "ZIP does not exist: $ZipPath"
    }
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("rtlzz_privacy_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    try {
        Expand-Archive -LiteralPath $ZipPath -DestinationPath $temp -Force
        Write-Host "Scanning expanded ZIP: $ZipPath"
        Test-Tree $temp
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($Issues.Count -gt 0) {
    foreach ($issue in $Issues) {
        Write-Error $issue
    }
    exit 1
}

Write-Host "Privacy scan passed."
