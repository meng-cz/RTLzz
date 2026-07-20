# RTLzz V2 Code Index

本文档是当前代码树的逐文件索引。项目现在只保留 V2 编译路径：

`S0 native AST -> S1 API norm -> S2 validate -> S3 statementize -> S4 CFG -> S5 unroll -> S6 inline -> S7 flatten -> S8 op norm -> S9 SSA -> S10 predicate -> S11 BEIR -> BEOPT -> RTL`

## 常用验证

```bash
cmake -S . -B build
cmake --build build --target predicate-expand -j2
cmake --build build -j2
git diff --check
python3 scripts/differential_rtl.py testv2/fixtures/int_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/flatten_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/controlflow_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/inline_misc.logic.cpp --top hls_main --cases 100
```

`scripts/differential_rtl.py` 通过 `predicate-expand --format portmeta` 获取端口元数据，通过 `--format rtl` 获取 RTL，然后使用 C++ oracle 与 Verilator 做随机输入差分。

## Build And Entry

### `CMakeLists.txt`
- 配置 C++17、libclang、`predicate-expand-objects` object library、`predicate-expand` CLI。
- 自动发现 `src/**/*.cpp` 作为编译器实现源文件。
- 自动发现 `testv2/*.cpp`，按文件名生成测试可执行文件并输出到 `build/testv2/`。

### `main/main.cpp`
- CLI 参数解析入口。
- 读取输入源文件，组装 `rtlzz::CompileOptions`。
- 支持 `--format rtl|beir|portmeta`，并转发 `--top`、`--vullib`、`--unroll-limit`、`--beopt`、`--clang-arg`。

### `src/rtlzz.hpp`
- Header-only API facade。
- 提供 `rtlzz::CompileOptions`、`rtlzz::CompileResult`。
- 提供 `compileToRtl`、`compileToBeir`、`compileToPortMetadata`，内部全部调用 `pipelinev2`。

## Pipeline V2

### `src/pipelinev2/PipelineV2.h`
- 声明 `PipelineConfig`、`PipelineResult`、`OutputKind` 和 `compile`。
- `OutputKind` 当前为 `Rtl`、`Beir`、`PortMetadata`。

### `src/pipelinev2/PipelineV2.cpp`
- 串接完整 V2 pipeline。
- 在 S7 后可提前输出端口 metadata。
- 在 S11 后调用 BEIR optimizer，再输出 BEIR 文本或 SystemVerilog。

## Shared V2 AST And Types

### `src/v2/V2Types.h`
- 定义 V2 surface type model：`TypeInfo`、`ParamDecl`、`ParamDirection`、`ParamPassingKind`、struct metadata。
- 当前定宽硬件整数统一表示为无符号存储 `Int<N>`；signedness 作为后续 operation operand view 传播。

### `src/v2/V2AST.h`
- 定义 S0 surface AST：`Expr`、`Stmt`、`FunctionAST`。
- 覆盖 literal、var、binary/unary、call、cast、ternary、field/array access、Int hardware surface ops、control-flow statements。

## Debug Support

### `src/debug/DebugLoc.h`
- 定义源位置 `DebugLoc`。

### `src/debug/RTLZZException.h`
- 声明 `ErrorContext`、`RTLZZException`、`ErrorContextGuard`。
- 支持阶段、文件、位置和消息组成的异常上下文栈。

### `src/debug/RTLZZException.cpp`
- 实现 debug context stack、异常格式化与 guard push/pop。

## S0 Native AST

### `src/s0ast/S0AST.h`
- 声明 S0 parse result、diagnostic、S0 internal program shape，以及 `parseProgram`、`surfaceAST`、`debugPrint`。

### `src/s0ast/S0AST.cpp`
- S0 program debug print、surface AST access、从 native builder 结果构造 `S0Program`。

### `src/s0ast/S0NativeASTBuilder.h`
- 声明 libclang native AST builder 入口与构建结果。

### `src/s0ast/S0NativeASTBuilder.cpp`
- 使用 libclang 解析 VUL-style C++。
- 完成 top/helper/lambda 抽取、scope/name 解析、类型识别、struct metadata、表达式/语句 surface AST 构造。
- 要求源级 top 为无参数 `void` 函数；收集文件级全局端口变量及 `#pragma input_port/output_port` 方向声明，并收敛为后续阶段使用的内部 `ParamDecl`。
- 拒绝未标注全局变量、无对应变量或重复冲突的端口 pragma、带初始化器或不受支持类型的全局端口。
- 直接产出 V2 `FunctionAST`，不依赖其他 AST builder。

### `src/s0ast/S0VulRecognizers.h`
- 声明 VUL/fixint 类型和 API 识别辅助枚举与函数。

### `src/s0ast/S0VulTypeRecognizer.cpp`
- 识别 `Int<N>`、bool、标准整数、array、struct 等 V2 支持类型。

### `src/s0ast/S0VulOpRecognizer.cpp`
- 识别 fixint 相关 API/operator surface form，供 S0 AST builder 构造 V2 AST。

### `src/s0ast/checklist*.md`
- S0 子阶段语义确认记录。

## S1 API Normalization

### `src/s1apinorm/S1NormedAST.h`
- 定义 S1 收敛后的 AST：独立 `Construct` statement，decl 无 init/init_args，hardware ops 一等化。

### `src/s1apinorm/S1APINorm.h`
- 声明 S1 normalize API、error/result/debug print。

### `src/s1apinorm/S1APINorm.cpp`
- 将 Int API call lowering 为 S1 hardware op。
- 将所有变量声明规范化为单纯 `Decl`，并按原 init 形式追加 assign/construct/call 等语义节点。

### `src/s1apinorm/checklist.md`
- S1 语义确认记录。

## S2 Validate

### `src/s2validate/S2Validate.h`
- 声明 S2 validator option/result/error。

### `src/s2validate/S2Validate.cpp`
- 校验 V2 支持的 C++ 子集。
- 检查名字、作用域、调用解析、helper/lambda/struct metadata、递归、非法参数和非法引用/指针/aggregate 端口。

### `src/s2validate/checklist.md`
- S2 语义确认记录。

## S3 Statementize

### `src/s3statementize/S3Statementize.h`
- 定义 statementized IR、symbol id、scope/debug metadata、statement-level call/construct/op。
- 从此阶段开始 symbol id 要求为 function 内唯一。

### `src/s3statementize/S3Statementize.cpp`
- 提升复杂表达式中的 call、副作用、构造和求值顺序敏感表达式。
- 生成显式临时变量和顺序 statement，附加 operand-level signed view。

### `src/s3statementize/checklist.md`
- S3 语义确认记录。

## S4 CFG

### `src/s4cfg/S4CFG.h`
- 定义 per-function CFG、basic block、edge、terminator、loop region metadata。
- loop region 抽象为 pre-test/post-test，并显式记录 init、condition、condition prelude、body 等 block。

### `src/s4cfg/S4CFG.cpp`
- 将 S3 顺序语句和结构化控制流 lowering 为 CFG。
- block 中只保留顺序语句和 statement-level call，控制流由 edge/terminator 表达。

### `src/s4cfg/S4LowerFunctionExits.cpp`
- 统一函数 return value slot 和 return exit。

### `src/s4cfg/checklist.md`
- S4 语义确认记录。

## S5 Unroll

### `src/s5unroll/S5Unroll.h`
- 声明 loop unroll result/options/debug print。

### `src/s5unroll/S5Unroll.cpp`
- 静态分析并完全展开当前硬件子集支持的循环。
- 支持 pre-test/post-test loop、nested loop、dynamic continue，以及 dynamic break 的 enable-flag masking。
- clone loop body 时为声明变量重新分配 function 内唯一 symbol id。

### `src/s5unroll/checklist.md`
- S5 语义确认记录。

## S6 Inline

### `src/s6inline/S6Inline.h`
- 声明 CFG-level inline result/options/debug print。

### `src/s6inline/S6Inline.cpp`
- 在 CFG 层 clone callee CFG、绑定参数、重命名局部、连接 return blocks 到 caller continuation。
- 支持 helper/lambda、多级调用、重载、loop body 内调用和递归检测。

### `src/s6inline/checklist.md`
- S6 语义确认记录。

## S7 Flatten

### `src/s7flatten/S7Flatten.h`
- 定义收窄后的 `FlattenedCFG` 与 `S7FlattenedProgram`。
- 只保留 scalar leaf symbol、flattened op、flattened stmt、flattened terminator、port group metadata。

### `src/s7flatten/S7Flatten.cpp`
- 将 struct、array、aggregate init/copy、field access、array access、动态索引读写 lowering 为 scalar leaf、lookup 或 guarded write。
- 维护输入/输出端口的原始数组形态和展开后的 leaf signal 列表。

### `src/s7flatten/checklist.md`
- S7 语义确认记录。

## S8 Operation Normalize

### `src/s8opnorm/S8Norm.h`
- 定义 normalized scalar operation IR、literal limb 表示、operation signed view metadata。

### `src/s8opnorm/S8Norm.cpp`
- 规范化 Int/builtin 整数语义、宽度扩展/截断、cast、slice、bit、concat、repeat、reduce、比较和算术。
- 将字面常量解析为 `vector<uint64_t> + valid_width`。
- 对常量除数 div/mod 做 lowering；二次幂转截取/移位，其他常量使用乘法/移位序列。

### `src/s8opnorm/checklist.md`
- S8 语义确认记录。

## S9 SSA

### `src/s9ssa/S9SSA.h`
- 定义 scalar CFG SSA IR、value id、operand、phi/merge、statement 和 terminator。

### `src/s9ssa/S9SSA.cpp`
- 对 S8 scalar CFG 做 SSA conversion。
- 插入/表示 phi 或等价 merge，拆分 lookup write 到逐元素 mux/guarded value。

### `src/s9ssa/checklist.md`
- S9 语义确认记录。

## S10 Predicate

### `src/s10predicate/S10Predicate.h`
- 定义 predicate-lowered value graph、definitions、final values 和 debug output。

### `src/s10predicate/S10Predicate.cpp`
- 将 SSA control/data merge lowering 为 predicate/value dependencies。
- 内部包含只读 verify/simplify 子阶段，检查 S10 输出结构一致性。

### `src/s10predicate/checklist.md`
- S10 语义确认记录。

## S11 BEIR

### `src/s11beir/S11BEIR.h`
- 声明 S10 到 BEIR conversion result/options/debug summary。

### `src/s11beir/S11BEIR.cpp`
- 将 S10 value graph 直接构造成 `beir::Program`。
- 恢复 V2 端口 metadata，数组端口保持 BEIR 原生 array port 形态。
- 将 lookup 转为 BEIR array lookup，保留后端识别常量查找表的机会。

### `src/s11beir/checklist.md`
- S11 结构差异和语义确认记录。

## Backend BEIR And RTL

### `src/backend/beir.hpp`
- 定义 BEIR program、signal、port、operand、operation、mutable program API。

### `src/backend/beir.cpp`
- BEIR text dump、validation、operation/type helpers、mutable builder 实现。

### `src/backend/beopt.hpp`
- 声明 BEIR optimizer options 和 `optimizeProgram`。

### `src/backend/beopt.cpp`
- 串接 BEIR optimization passes。

### `src/backend/beopt_constant.hpp`
- 常量传播、常量折叠和 literal 简化。

### `src/backend/beopt_algebraic.hpp`
- 代数化简。

### `src/backend/beopt_assign_chains.hpp`
- assignment chain 简化。

### `src/backend/beopt_cse.hpp`
- common subexpression elimination。

### `src/backend/beopt_dce.hpp`
- dead code elimination。

### `src/backend/beopt_predicate.hpp`
- predicate/guard 相关 BEIR 优化。

### `src/backend/beopt_width.hpp`
- width 相关优化和裁剪；支持 operand signed view 影响的扩展语义。

### `src/backend/rtlgen.hpp`
- 声明 SystemVerilog emitter。

### `src/backend/rtlgen.cpp`
- 将 BEIR program emit 为 synthesizable SystemVerilog。
- 支持 scalar/array ports、BEIR lookup、assign/operation lowering。

## Tests And Fixtures

### `testv2/s1apinorm_test.cpp`
- S1 API lowering、decl normalization、construct/call/assign init 语义测试。

### `testv2/s2validate_test.cpp`
- S2 支持子集校验、非法 proxy/引用/struct port/unknown call/递归测试。

### `testv2/s3statementize_test.cpp`
- S3 expression lifting、call lifting、temp symbol、signed view attachment 测试。

### `testv2/s4cfg_test.cpp`
- 手写 AST 到 S4 CFG 的控制流结构测试。

### `testv2/s4cfg_integration_test.cpp`
- C++ fixture 经 S0-S4 的 CFG 集成测试。

### `testv2/s5unroll_test.cpp`
- S5 loop unroll 单元测试。

### `testv2/s5unroll_integration_test.cpp`
- C++ fixture 经 S0-S5 的 loop unroll 集成测试。

### `testv2/s6inline_test.cpp`
- S6 helper/lambda inline、overload、多级调用、递归检测测试。

### `testv2/s7flatten_test.cpp`
- S7 struct/array flatten、aggregate、dynamic lookup/write 测试。

### `testv2/s8opnorm_test.cpp`
- S8 operation normalization、literal、signed view、div/mod lowering 测试。

### `testv2/s9ssa_test.cpp`
- S9 SSA conversion、merge、lookup write lowering 测试。

### `testv2/s10predicate_test.cpp`
- S10 predicate lowering 和 internal verify/simplify 测试。

### `testv2/s11beir_test.cpp`
- S11 BEIR conversion、array port、lookup/operation mapping 测试。

### `testv2/fixtures/int_misc.logic.cpp`
- End-to-end integer/fixint API fixture：arithmetic、bit op、shift、compare、slice、pick、cat/repeat/reduce、cast、enum、standard integer mixing。

### `testv2/fixtures/flatten_misc.logic.cpp`
- End-to-end struct/array/aggregate/dynamic index/constant lookup fixture。

### `testv2/fixtures/controlflow_misc.logic.cpp`
- End-to-end nested if/switch/loop、dynamic break/continue、early-return fixture。

### `testv2/fixtures/inline_misc.logic.cpp`
- End-to-end helper/lambda inline、overload、parameter/return fixture。

### `testv2/fixtures/s*/...`
- Stage-specific C++ fixtures used by integration tests.

## Scripts

### `scripts/differential_rtl.py`
- V2 RTL differential harness。
- 生成 port metadata、RTL、C++ oracle 和 Verilator testbench；oracle 直接写入源文件全局 input port、调用无参 top、读取全局 output port，并比较随机输入下的输出。

## Third Party And Docs

### `third_party/vulsim/vullib/fixint.hpp`
- VUL fixed-width `Int<N>` runtime header used by source fixtures and C++ oracle。

### `third_party/vulsim/vullib/common.h`
- VUL common integer aliases and helpers。

### `docs/THIRD_PARTY_NOTICES.md`
- Third-party notices。

### `roadmap.md`
- V2 pipeline stage responsibilities and long-term compiler route。
