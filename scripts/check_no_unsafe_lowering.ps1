param(
    [string[]]$Files = @(
        "src\ast\ASTBuilder.cpp",
        "src\ast\ASTBuilderDriver.cpp",
        "src\transform\Normalize.cpp",
        "src\normalize\NormalizeDriver.cpp"
    )
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath ".").Path
$patterns = @(
    "parseSimpleVarAssignment",
    "lhsTextBeforeAssign",
    "parseTextArrayAccess",
    "parseBitSelectCall",
    "assignmentLhsIdentifierFromTokens",
    "leadingIdentifierFromTokens",
    "arrayAccessFromBracketTokens",
    "cursorText",
    "cursorSourceSlice",
    "tokenExpr",
    "parseOpaqueArrayVar",
    "parseOpaqueHwMethodVar",
    "make_literal(text",
    "literal_value = text",
    'text.find(".setnext"',
    'text.find(".cat"',
    'text.find(".repeat"',
    'text.find(".range_at"',
    'text.find(".bit_at"'
)

$allowedReasons = @(
    "UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false",
    "UNSAFE_TEXT_FALLBACK_ALLOW: static lookup-table initializer extraction",
    "UNSAFE_TEXT_FALLBACK_ALLOW: declaration initializer presence check",
    "UNSAFE_TEXT_FALLBACK_ALLOW: C array parameter dimension recovery",
    "UNSAFE_TEXT_FALLBACK_ALLOW: diagnostics only, not lowering",
    "UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback",
    "UNSAFE_TEXT_FALLBACK_ALLOW: disabled lambda text fallback",
    "UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering",
    "UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering"
)

$guardedCallHelpers = @(
    "parseSimpleVarAssignment",
    "lhsTextBeforeAssign",
    "parseTextArrayAccess",
    "parseBitSelectCall",
    "assignmentLhsIdentifierFromTokens",
    "tokenExpr",
    "parseOpaqueArrayVar",
    "parseOpaqueHwMethodVar"
)

$tokenRecoveryHelpers = @{
    "leadingIdentifierFromTokens" = "UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering"
    "arrayAccessFromBracketTokens" = "UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering"
}

$unguardedAllowedReasons = @(
    "UNSAFE_TEXT_FALLBACK_ALLOW: static lookup-table initializer extraction",
    "UNSAFE_TEXT_FALLBACK_ALLOW: declaration initializer presence check",
    "UNSAFE_TEXT_FALLBACK_ALLOW: C array parameter dimension recovery",
    "UNSAFE_TEXT_FALLBACK_ALLOW: diagnostics only, not lowering",
    "UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering",
    "UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering"
)

function Fail([string]$Message) {
    throw $Message
}

function IsHelperDefinition([string]$Line, [string]$Name) {
    $escaped = [Regex]::Escape($Name)
    return $Line -match "^\s*(static\s+)?[A-Za-z_][A-Za-z0-9_:<>\s\*&]+?\s+$escaped\s*\("
}

function HasAllowedReason([string]$Line, [string[]]$Reasons) {
    foreach ($reason in $Reasons) {
        if ($Line.Contains($reason)) {
            return $true
        }
    }
    return $false
}

function IsGuardedByUnsafeFlag([string[]]$Lines, [int]$Index) {
    if ($Lines[$Index].Contains("allowUnsafeTextFallback")) {
        return $true
    }
    for ($j = [Math]::Max(0, $Index - 3); $j -lt $Index; ++$j) {
        if ($Lines[$j].Contains("allowUnsafeTextFallback")) {
            return $true
        }
    }
    return $false
}

foreach ($file in $Files) {
    $path = Join-Path $repo $file
    if (-not (Test-Path -LiteralPath $path)) {
        Fail "Missing source file for unsafe lowering check: $file"
    }

    $lines = Get-Content -LiteralPath $path
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $line = $lines[$i]
        if ($line.Contains("UNSAFE_TEXT_FALLBACK_ALLOW_BEGIN") -or $line.Contains("UNSAFE_TEXT_FALLBACK_ALLOW_END")) {
            Fail "Broad unsafe fallback allow blocks are forbidden in $file at line $($i + 1). Use a narrow line-level allow comment."
        }

        $isAllowed = $false
        foreach ($reason in $allowedReasons) {
            if ($line.Contains($reason)) {
                $isAllowed = $true
                break
            }
        }
        if ($line.Contains("UNSAFE_TEXT_FALLBACK_ALLOW") -and -not $isAllowed) {
            Fail "Unknown unsafe fallback allow reason in $file at line $($i + 1)."
        }

        foreach ($helper in $guardedCallHelpers) {
            if (-not $line.Contains("$helper(")) {
                continue
            }
            if (IsHelperDefinition $line $helper) {
                continue
            }
            $guarded = IsGuardedByUnsafeFlag $lines $i
            $explicitAllowed = HasAllowedReason $line $unguardedAllowedReasons
            if (-not $guarded -and -not $explicitAllowed) {
                Fail "Unsafe helper call '$helper' in $file at line $($i + 1) is not guarded by allowUnsafeTextFallback."
            }
            if (-not $guarded -and $line.Contains("UNSAFE_TEXT_FALLBACK_ALLOW") -and -not $explicitAllowed) {
                Fail "Unsafe helper call '$helper' in $file at line $($i + 1) has an allow comment but no allowUnsafeTextFallback guard."
            }
        }

        foreach ($helper in $tokenRecoveryHelpers.Keys) {
            if (-not $line.Contains("$helper(")) {
                continue
            }
            if (-not $line.Contains($tokenRecoveryHelpers[$helper])) {
                Fail "Token recovery helper '$helper' in $file at line $($i + 1) is outside its narrow libclang recovery allowlist."
            }
            if ($line -match "setnext|output|cat|repeat|reduce|range_at|bit_at|parseSimpleVarAssignment|lhsTextBeforeAssign") {
                Fail "Token recovery helper '$helper' in $file at line $($i + 1) is used in a forbidden lowering context."
            }
        }

        if ($isAllowed) {
            continue
        }
        foreach ($pattern in $patterns) {
            if ($line.Contains($pattern)) {
                Fail "Unsafe text fallback '$pattern' in $file at line $($i + 1). Add a narrow AST path or mark an explicit allowlist line."
            }
        }
    }
}

Write-Host "Unsafe lowering check passed."
