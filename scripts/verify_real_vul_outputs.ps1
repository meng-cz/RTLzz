param(
    [string[]]$JsonPath = @(),
    [string[]]$TextPath = @()
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) { throw $Message }

$badTokens = @(
    "lambda", "auto", "[&]", "setnext(", "output(", "operator()",
    "range_at(", "bit_at(", "repeat(", ".cat(", "Cat(",
    "source_text", "unlowered", "unlowered call",
    "field_access", "array_access"
)

foreach ($file in ($JsonPath + $TextPath)) {
    if (-not (Test-Path -LiteralPath $file)) { Fail "Missing output file: $file" }
    $raw = Get-Content -LiteralPath $file -Raw
    foreach ($token in $badTokens) {
        if ($raw.Contains($token)) { Fail "Invalid residual token '$token' in $file" }
    }
}

foreach ($jsonFile in $JsonPath) {
    $raw = Get-Content -LiteralPath $jsonFile -Raw
    try { $json = $raw | ConvertFrom-Json } catch { Fail "Invalid JSON: $jsonFile" }
    foreach ($field in @("schema_version", "tool_version", "build_commit", "function", "inputs", "outputs", "symbols", "assignments", "output_expressions", "lookup_tables", "diagnostics", "output_paired_controls")) {
        if (-not ($json.PSObject.Properties.Name -contains $field)) { Fail "Missing JSON field '$field' in $jsonFile" }
    }
    if ([string]$json.schema_version -ne "gpef-predicate-json-v1") {
        Fail "Unsupported JSON schema_version '$($json.schema_version)' in $jsonFile"
    }
    if ($raw.Contains('"kind": "elided"') -or $raw.Contains('<expr elided>')) { Fail "Elided expression in $jsonFile" }
    if ($json.output_expressions.Count -eq 0) { Fail "Missing output expressions in $jsonFile" }

    function Test-ExprNode($node, [string]$path) {
        if ($null -eq $node) { return }
        if ($node -is [string] -or $node -is [ValueType]) { return }
        if ($node -is [System.Collections.IEnumerable] -and -not ($node -is [System.Management.Automation.PSCustomObject])) {
            $i = 0
            foreach ($item in $node) {
                Test-ExprNode $item "$path[$i]"
                $i++
            }
            return
        }

        $props = $node.PSObject.Properties.Name
        if ($props -contains "kind") {
            $kind = [string]$node.kind
            if ($kind -eq "elided") { Fail "Elided expression at $path in $jsonFile" }
            if ($kind -eq "field_access") { Fail "Unlowered field_access at $path in $jsonFile" }
            if ($kind -eq "array_access") { Fail "Unlowered array_access at $path in $jsonFile" }
            if ($kind -eq "call") {
                $callee = [string]$node.callee
                if (@("lookup", "__dynamic_range_at", "__dynamic_bit_at") -notcontains $callee) {
                    Fail "Unlowered call '$callee' at $path in $jsonFile"
                }
            }
            if ($kind -ne "literal" -and ($props -contains "width")) {
                if ([int]$node.width -le 0) { Fail "Unknown width at $path in $jsonFile" }
            }
            if (($kind -eq "slice" -or $kind -eq "write_slice") -and
                ($props -contains "hi") -and ($props -contains "lo")) {
                if ([int]$node.lo -lt 0 -or [int]$node.hi -lt [int]$node.lo) {
                    Fail "Invalid slice range at $path in $jsonFile"
                }
                if (($props -contains "base") -and $null -ne $node.base -and
                    ($node.base.PSObject.Properties.Name -contains "width") -and
                    [int]$node.base.width -gt 0 -and [int]$node.hi -ge [int]$node.base.width) {
                    Fail "Slice range out of bounds at $path in $jsonFile"
                }
            }
            if (($kind -eq "bit_select" -or $kind -eq "write_bit") -and ($props -contains "bit")) {
                if ([int]$node.bit -lt 0) { Fail "Invalid bit index at $path in $jsonFile" }
                if (($props -contains "base") -and $null -ne $node.base -and
                    ($node.base.PSObject.Properties.Name -contains "width") -and
                    [int]$node.base.width -gt 0 -and [int]$node.bit -ge [int]$node.base.width) {
                    Fail "Bit index out of bounds at $path in $jsonFile"
                }
            }
        }

        foreach ($prop in $node.PSObject.Properties) {
            if ($prop.Value -is [string] -or $prop.Value -is [ValueType]) { continue }
            Test-ExprNode $prop.Value "$path.$($prop.Name)"
        }
    }

    foreach ($assignment in $json.assignments) {
        Test-ExprNode $assignment.guard_expr "assignments.guard_expr"
        Test-ExprNode $assignment.target_expr "assignments.target_expr"
        Test-ExprNode $assignment.value_expr "assignments.value_expr"
    }
    foreach ($prop in $json.output_expressions.PSObject.Properties) {
        $out = $prop.Value
        foreach ($required in @("has_default_policy", "default_applied", "default_value", "default_reason", "default_policy", "assignment_coverage", "default_guard_relation", "paired_control", "valid_when", "inactive_value", "inactive_semantics", "guard_relation")) {
            if (-not ($out.PSObject.Properties.Name -contains $required)) {
                Fail "Missing output default field '$required' for '$($prop.Name)' in $jsonFile"
            }
        }
        Test-ExprNode $out.expr_tree "output_expressions.$($prop.Name)"
    }
}

Write-Host "Generic output verification passed."
