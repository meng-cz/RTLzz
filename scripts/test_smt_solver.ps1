param(
    [string]$BuildDir = "build",
    [string]$OutputDir = "",
    [string]$ConsSource = "",
    [switch]$RequireZ3
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $BuildPath "smt_solver_tests"
}
$OutputDir = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $Root $OutputDir }
$IncludeDir = Join-Path $Root "third_party\vulsim\vullib"

function Fail([string]$Message) { throw $Message }

$Z3 = Get-Command z3 -ErrorAction SilentlyContinue
if (-not $Z3) {
    if ($RequireZ3) {
        throw "Z3 is required for strict SMT validation but was not found in PATH."
    }
    Write-Host "[SKIP] SMT solver validation skipped: z3 not found in PATH."
    Write-Host "[SKIP] Install Z3 and rerun scripts/test_smt_solver.ps1 to validate SMT-LIB parse/type correctness."
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Write-Utf8File([string]$Path, [string]$Content) {
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function Invoke-Z3Case([string]$Name, [string]$Source, [string]$Top = "hls_main", [string[]]$ExtraClangArg = @()) {
    $SmtPath = Join-Path $OutputDir "$Name.smt2"
    $ClangArgs = @("-I$IncludeDir", "-std=c++20") + $ExtraClangArg
    Write-Host "Running SMT solver case $Name..."
    & (Join-Path $PSScriptRoot "run_real_case.ps1") `
        -Source $Source `
        -Top $Top `
        -Format smt `
        -Output $SmtPath `
        -BuildDir $BuildDir `
        -ClangArg $ClangArgs
    if ($LASTEXITCODE -ne 0) { Fail "SMT generation failed for $Name with exit code $LASTEXITCODE" }

    $stdout = Join-Path $OutputDir "$Name.z3.stdout.txt"
    $stderr = Join-Path $OutputDir "$Name.z3.stderr.txt"
    & $Z3.Source -smt2 $SmtPath > $stdout 2> $stderr
    $exit = $LASTEXITCODE
    $outText = if (Test-Path $stdout) { Get-Content -LiteralPath $stdout -Raw } else { "" }
    $errText = if (Test-Path $stderr) { Get-Content -LiteralPath $stderr -Raw } else { "" }
    $combined = "$outText`n$errText"
    if ($exit -ne 0) {
        Fail "Z3 failed for $Name with exit code $exit`n$combined"
    }
    foreach ($bad in @("unknown constant", "Sort mismatch", "wrong sort", "arguments of the wrong sort", "operator is applied to arguments of the wrong sort")) {
        if ($combined -match [regex]::Escape($bad)) {
            Fail "Z3 reported SMT type/name error for $Name`: $bad`n$combined"
        }
    }
    if ($outText -notmatch "(?m)^\s*sat\s*$") {
        Fail "Z3 did not report sat for $Name`nstdout:`n$outText`nstderr:`n$errText"
    }
}

function Invoke-Z3CaseWithExtraAssertions([string]$Name,
                                           [string]$Source,
                                           [string[]]$ExtraAssertions,
                                           [string]$Expected = "unsat",
                                           [string]$Top = "hls_main",
                                           [string[]]$ExtraClangArg = @()) {
    $SmtPath = Join-Path $OutputDir "$Name.smt2"
    $ClangArgs = @("-I$IncludeDir", "-std=c++20") + $ExtraClangArg
    Write-Host "Running SMT solver semantic case $Name..."
    & (Join-Path $PSScriptRoot "run_real_case.ps1") `
        -Source $Source `
        -Top $Top `
        -Format smt `
        -Output $SmtPath `
        -BuildDir $BuildDir `
        -ClangArg $ClangArgs
    if ($LASTEXITCODE -ne 0) { Fail "SMT generation failed for $Name with exit code $LASTEXITCODE" }

    $raw = Get-Content -LiteralPath $SmtPath -Raw
    $raw = $raw -replace "\\(check-sat\\)", ""
    foreach ($assertion in $ExtraAssertions) {
        $raw += "`n$assertion"
    }
    $raw += "`n(check-sat)`n"
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($SmtPath, $raw, $Utf8NoBom)

    $stdout = Join-Path $OutputDir "$Name.z3.stdout.txt"
    $stderr = Join-Path $OutputDir "$Name.z3.stderr.txt"
    & $Z3.Source -smt2 $SmtPath > $stdout 2> $stderr
    $exit = $LASTEXITCODE
    $outText = if (Test-Path $stdout) { Get-Content -LiteralPath $stdout -Raw } else { "" }
    $errText = if (Test-Path $stderr) { Get-Content -LiteralPath $stderr -Raw } else { "" }
    $combined = "$outText`n$errText"
    if ($exit -ne 0) {
        Fail "Z3 failed for $Name with exit code $exit`n$combined"
    }
    foreach ($bad in @("unknown constant", "Sort mismatch", "wrong sort", "arguments of the wrong sort", "operator is applied to arguments of the wrong sort")) {
        if ($combined -match [regex]::Escape($bad)) {
            Fail "Z3 reported SMT type/name error for $Name`: $bad`n$combined"
        }
    }
    if ($outText -notmatch "(?m)^\s*$Expected\s*$") {
        Fail "Z3 did not report $Expected for $Name`nstdout:`n$outText`nstderr:`n$errText"
    }
}

try {
    $Unconditional = Join-Path $OutputDir "unconditional_bool_guard.logic.cpp"
    Write-Utf8File $Unconditional @'
#include <uint.hpp>

void hls_main(bool& output__vld__, Int<8>& output_s__) {
    output__vld__ = false;
    output_s__ = Int<8>(0);
}
'@
    Invoke-Z3Case "unconditional_bool_guard" $Unconditional

    $Declarations = Join-Path $OutputDir "output_symbol_declarations.logic.cpp"
    Write-Utf8File $Declarations @'
#include <uint.hpp>

void hls_main(bool fire,
              Int<8> payload,
              bool& output__vld__,
              Int<8>& output_s__,
              bool& wen_sum__,
              Int<8>& wdata_sum__) {
    output__vld__ = fire;
    output_s__ = payload;
    wen_sum__ = fire;
    wdata_sum__ = payload;
}
'@
    Invoke-Z3Case "output_symbol_declarations" $Declarations

    $BoolCast = Join-Path $OutputDir "bool_cast_semantics.logic.cpp"
    Write-Utf8File $BoolCast @'
#include <uint.hpp>

void hls_main(bool flag, Int<8> word, Int<8>& out_word, bool& out_bool) {
    out_word = Int<8>(flag);
    out_bool = word.reduce_or();
}
'@
    Invoke-Z3CaseWithExtraAssertions "bool_cast_flag_false" $BoolCast @(
        "(assert (= flag_0 false))",
        "(assert (not (= out_word (_ bv0 8))))"
    )
    Invoke-Z3CaseWithExtraAssertions "bool_cast_flag_true" $BoolCast @(
        "(assert (= flag_0 true))",
        "(assert (not (= out_word (_ bv1 8))))"
    )
    Invoke-Z3CaseWithExtraAssertions "bool_cast_word_zero" $BoolCast @(
        "(assert (= word_0 (_ bv0 8)))",
        "(assert out_bool)"
    )
    Invoke-Z3CaseWithExtraAssertions "bool_cast_word_one" $BoolCast @(
        "(assert (= word_0 (_ bv1 8)))",
        "(assert (not out_bool))"
    )
    Invoke-Z3CaseWithExtraAssertions "bool_cast_word_255" $BoolCast @(
        "(assert (= word_0 (_ bv255 8)))",
        "(assert (not out_bool))"
    )

    $NegativeLiterals = Join-Path $OutputDir "negative_literals.logic.cpp"
    Write-Utf8File $NegativeLiterals @'
#include <uint.hpp>

void hls_main(Int<8>& m1, Int<8>& m128, Int<8>& p127, Int<8>& p255) {
    m1 = Int<8>(-1);
    m128 = Int<8>(-128);
    p127 = Int<8>(127);
    p255 = Int<8>(255);
}
'@
    Invoke-Z3CaseWithExtraAssertions "negative_literals" $NegativeLiterals @(
        "(assert (or (not (= m1 (_ bv255 8))) (not (= m128 (_ bv128 8))) (not (= p127 (_ bv127 8))) (not (= p255 (_ bv255 8)))))"
    )

    $SignedCompare = Join-Path $OutputDir "signed_compare.logic.cpp"
    Write-Utf8File $SignedCompare @'
#include <uint.hpp>

void hls_main(bool& lt, bool& gt) {
    lt = Int<8>(-1).sint() < Int<8>(1).sint();
    gt = Int<8>(-1).sint() > Int<8>(1).sint();
}
'@
    Invoke-Z3CaseWithExtraAssertions "signed_compare" $SignedCompare @(
        "(assert (or (not lt) gt))"
    )

    $SignedDivRem = Join-Path $OutputDir "signed_div_rem.logic.cpp"
    Write-Utf8File $SignedDivRem @'
#include <cstdint>

void hls_main(uint64_t lhs, uint64_t rhs, uint64_t& quot, uint64_t& rem) {
    quot = static_cast<uint64_t>(
        static_cast<int64_t>(lhs) / static_cast<int64_t>(rhs));
    rem = static_cast<uint64_t>(
        static_cast<int64_t>(lhs) % static_cast<int64_t>(rhs));
}
'@
    Invoke-Z3CaseWithExtraAssertions "signed_div_rem" $SignedDivRem @(
        "(assert (= lhs_0 (_ bv18446744073709551609 64)))",
        "(assert (= rhs_0 (_ bv2 64)))",
        "(assert (or (not (= quot (_ bv18446744073709551613 64))) (not (= rem (_ bv18446744073709551615 64)))))"
    )

    $BuiltinCpp = Join-Path $OutputDir "builtin_cpp_semantics.logic.cpp"
    Write-Utf8File $BuiltinCpp @'
#include <cstdint>

void hls_main(bool sel,
              int8_t a,
              uint8_t b,
              int32_t c,
              uint32_t d,
              int16_t& post_inc,
              int16_t& pre_inc,
              int16_t& post_dec,
              int16_t& pre_dec,
              int16_t& add,
              int16_t& sub,
              int16_t& ush,
              int16_t& inv,
              int16_t& neg,
              uint64_t& wrap_add,
              uint64_t& wrap_mul,
              int16_t& choice,
              bool& cmp8,
              bool& cmp32) {
    int x = 5;
    x++;
    post_inc = x;
    int y = 5;
    ++y;
    pre_inc = y;
    int z = 5;
    z--;
    post_dec = z;
    int w = 5;
    --w;
    pre_dec = w;
    add = int8_t(-1) + 0;
    sub = uint8_t(0) - 1;
    ush = uint8_t(0x80) << 1;
    inv = ~uint8_t(0);
    neg = -int8_t(-128);
    wrap_add = uint32_t(0xffffffffu) + uint32_t(1);
    wrap_mul = uint32_t(0xffffffffu) * uint32_t(2);
    choice = sel ? a : b;
    cmp8 = a < b;
    cmp32 = c < d;
}
'@
    Invoke-Z3CaseWithExtraAssertions "builtin_cpp_constants" $BuiltinCpp @(
        "(assert (or (not (= post_inc (_ bv6 16))) (not (= pre_inc (_ bv6 16))) (not (= post_dec (_ bv4 16))) (not (= pre_dec (_ bv4 16))) (not (= add (_ bv65535 16))) (not (= sub (_ bv65535 16))) (not (= ush (_ bv256 16))) (not (= inv (_ bv65535 16))) (not (= neg (_ bv128 16))) (not (= wrap_add (_ bv0 64))) (not (= wrap_mul (_ bv4294967294 64)))))"
    )
    Invoke-Z3CaseWithExtraAssertions "builtin_cpp_usual_conversions" $BuiltinCpp @(
        "(assert (= sel_0 false))",
        "(assert (= a_0 (_ bv255 8)))",
        "(assert (= b_0 (_ bv200 8)))",
        "(assert (= c_0 (_ bv4294967295 32)))",
        "(assert (= d_0 (_ bv1 32)))",
        "(assert (or (not (= choice (_ bv200 16))) (not cmp8) cmp32))"
    )

    $DynamicSelect = Join-Path $OutputDir "dynamic_select.logic.cpp"
    Write-Utf8File $DynamicSelect @'
#include <uint.hpp>

void hls_main(Int<8> word, Int<3> idx, Int<4>& range, bool& bit) {
    range = word.range_at<4>(idx);
    bit = word.bit_at(idx);
}
'@
    Invoke-Z3CaseWithExtraAssertions "dynamic_select" $DynamicSelect @(
        "(assert (= word_0 (_ bv182 8)))",
        "(assert (= idx_0 (_ bv2 3)))",
        "(assert (or (not (= range (_ bv13 4))) (not bit)))"
    )

    $WriteReduce = Join-Path $OutputDir "write_reduce.logic.cpp"
    Write-Utf8File $WriteReduce @'
#include <uint.hpp>

void hls_main(Int<8> a, Int<8>& out, bool& any, bool& all, bool& parity, Int<8>& repeated, Int<8>& joined) {
    out = a;
    out(3, 0) = Int<4>(0xa);
    any = a.reduce_or();
    all = a.reduce_and();
    parity = a.reduce_xor();
    repeated = a(1, 0).repeat<4>();
    joined = a(7, 4).cat(a(3, 0));
}
'@
    Invoke-Z3Case "write_reduce_concat_repeat" $WriteReduce

    $ConsClangArgs = @()
    $ConsTop = "hls_main"
    if ([string]::IsNullOrWhiteSpace($ConsSource)) {
        $DesktopCons = Join-Path ([Environment]::GetFolderPath("Desktop")) "cons.logic(1).cpp"
        if (Test-Path -LiteralPath $DesktopCons) {
            $ConsSource = $DesktopCons
            $ConsTop = "LogicSubModule_Consumer_top_cons"
            $ConsClangArgs = @("-include", "uint.hpp")
        }
    }
    if ([string]::IsNullOrWhiteSpace($ConsSource)) {
        $ConsSource = Join-Path $OutputDir "cons_logic_smt_solver_parse.logic.cpp"
        Write-Utf8File $ConsSource @'
#include <cstdint>
#include <uint.hpp>

using uint8 = uint8_t;

struct __RegProxy_uint8_t__sum {
    const Int<8>& rdata;
    bool& wen;
    Int<8>& wdata;
    __RegProxy_uint8_t__sum(const Int<8>& r, bool& e, Int<8>& d) : rdata(r), wen(e), wdata(d) {}
    uint8 get() const {
        uint8 value;
        value = rdata(7, 0);
        return value;
    }
    template <uint32_t P = 0>
    void setnext(const uint8& value) {
        Int<8> packed;
        packed(7, 0) = value;
        wdata = packed;
        wen = true;
    }
};

struct __ReqHelper__output {
    bool& vld_ports;
    Int<8>& arg_s;
    __ReqHelper__output(bool& v, Int<8>& s) : vld_ports(v), arg_s(s) {}
    void call(uint8 value) {
        vld_ports = true;
        arg_s(7, 0) = value;
    }
};

void hls_main(bool recv__vld__,
              Int<8> recv_d__,
              const Int<8>& rdata_sum__,
              bool& wen_sum__,
              Int<8>& wdata_sum__,
              bool& output__vld__,
              Int<8>& output_s__) {
    __RegProxy_uint8_t__sum sum(rdata_sum__, wen_sum__, wdata_sum__);
    __ReqHelper__output output(output__vld__, output_s__);
    bool rdy = true;
    uint8 d = recv_d__(7, 0);
    if (rdy && recv__vld__) {
        sum.setnext<0>(d);
        output.call(sum.get());
    }
}
'@
    }
    Invoke-Z3Case "cons_logic_smt_solver_parse" $ConsSource $ConsTop $ConsClangArgs

    Write-Host "SMT solver validation passed."
} catch {
    Write-Error "SMT solver validation failed: $($_.Exception.Message)"
    exit 1
}
