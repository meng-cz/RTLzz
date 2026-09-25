# RTLzz V2 Code Index

本文档是当前代码树的逐文件索引。项目现在只保留 V2 编译路径：

`S0 native AST -> S1 API norm -> S2 validate -> S3 statementize -> S4 CFG -> S5 unroll -> S6 inline -> S7 flatten -> S8 op norm -> S9 SSA -> S10 predicate -> S11 BEIR -> BEOPT -> RTL (native or CIRCT)`

## 回归测试流程

```bash
cmake -S . -B build
cmake --build build --target predicate-expand -j2
cmake --build build -j2
git diff --check
python3 scripts/differential_rtl.py testv2/fixtures/int_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/flatten_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/controlflow_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/inline_misc.logic.cpp --top hls_main --cases 100

# 真实硬件模块摘出的组合逻辑 example 回归；每个文件使用一个 LogicSubModule_* top。
for src in testv2/example/*.logic.cpp; do
  python3 scripts/differential_rtl.py "$src" --top 'LogicSubModule_*' --cases 100
done
```

`scripts/differential_rtl.py` 通过 `predicate-expand --format portmeta` 获取端口元数据，通过 `--format rtl` 获取 RTL，然后使用 C++ oracle 与 Verilator 做随机输入差分。`testv2/example/*.logic.cpp` 中的每个输入文件代表一个从真实硬件中摘出的模块组合逻辑函数，使用目标函数名通配 `--top 'LogicSubModule_*'` 对每个 example 进行 RTL-DIFF 差分验证。

## Build And Entry

### `CMakeLists.txt`
- 配置 C++17、libclang、`predicate-expand-objects` object library、`predicate-expand` CLI。
- 自动发现 `src/**/*.cpp` 作为编译器实现源文件。
- 自动发现 `testv2/*.cpp`，按文件名生成测试可执行文件并输出到 `build/testv2/`。

### `main/main.cpp`
- CLI 参数解析入口。
- `--release` / `-r` 仅用于 RTL 输出，禁用调试文件及失败快照，优先于显式 debug 文件选项。
- 读取输入源文件，组装 `rtlzz::CompileOptions`。
- 支持 `--format rtl|beir|portmeta`，并转发 `--top`、`--vullib`、`--unroll-limit`、`--beopt`、`--clang-arg`。

### `src/rtlzz.hpp`
- Header-only API facade。
- 提供 `rtlzz::CompileOptions`、`rtlzz::CompileResult`。
- `CompileOptions::progress_callback` 是可选状态回调，逐阶段报告 frontend、逐轮报告 backend optimizer；未设置时 API 保持静默。
- 提供 `compileToRtl`、`compileToBeir`、`compileToPortMetadata`，内部全部调用 `pipelinev2`。

## Pipeline V2

### `src/pipelinev2/PipelineV2.h`
- 声明 `PipelineConfig`、`PipelineResult`、`OutputKind` 和 `compile`。
- `OutputKind` 当前为 `Rtl`、`Beir`、`PortMetadata`。
- `PipelineConfig::progress_callback` 由嵌入工具提供，用于报告 frontend 当前阶段和 backend 当前优化轮次。

### `src/pipelinev2/PipelineV2.cpp`
- 串接完整 V2 pipeline。
- 在 S0 至 S11 每个 frontend 阶段开始前触发可选进度回调，并转发 BEIR optimizer 的实际轮次。
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
- helper/lambda 可直接访问全局端口；通过 Clang 声明身份区分同名遮蔽，沿调用图传播端口依赖，再提升为隐式参数并补全调用实参。
- 收集源文件内实际使用的函数模板特化并赋予独立 helper 身份，使特化后的常量模板参数、端口依赖和调用关系进入正常 inline 流程。
- 区分无初始化的 `std::array` 声明与显式 `= {}` aggregate 初始化；前者不隐式补零，读取未定义值由 SSA 明确报错。
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
- `ConstRef` 的 lvalue 实参直接绑定为 caller lvalue alias；全局输入端口提升出的只读聚合参数不会在每个 helper 调用点复制并于 S7 重复展平。rvalue const-ref 仍创建独立存储。

### `src/s6inline/checklist.md`
- S6 语义确认记录。

## S7 Flatten

### `src/s7flatten/S7Flatten.h`
- 定义收窄后的 `FlattenedCFG` 与 `S7FlattenedProgram`。
- 只保留 scalar leaf symbol、flattened op、flattened stmt、flattened terminator、port group metadata。
- `FlattenOptions::max_leaf_symbols` 默认 `0`，表示不施加人为 leaf-symbol 数量上限；可为不可信输入显式设置资源上限。

### `src/s7flatten/S7Flatten.cpp`
- 将 struct、array、aggregate init/copy、field access、array access、动态索引读写 lowering 为 scalar leaf、lookup 或 guarded write。
- 聚合构造参数允许来自动态数组元素：先逐 leaf materialize lookup 临时值，再写入构造目标，保持完整的源求值先于目标写入。
- leaf-symbol 计数使用无溢出的 `size_t` 边界判断；默认仅受地址空间与内部 `SymbolId` 表示范围约束。
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

### `src/s8opnorm/S81StateUpgrade.hpp/.cpp`
- S8 结束、S9 SSA 前的分支局部组合计算提升：从最深分支向外处理，复制定义为带 `__s81_state_up_` 前缀的新临时信号，并递归重命名单前驱后代中的使用。
- 仅提升无副作用、总定义的 Assign/静态组合 Op；动态读写、数组查找、端口写入、重定义或跨分支合流可见的值均保守保留在原分支中。

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
- 只读校验中的布尔表达式通过稳定节点 ID 规范化 `And/Or`、hash-consing 复用节点；结构等价比较缓存节点对，蕴含缓存先于结构比较和 BDD 查询，以避免深层共享 guard DAG 的指数重复遍历。

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
- 定义 BEIR program、signal、port、operand、operation、mutable program API；`OperationKind::Case` 使用 `[condition, value, ..., default]` 布局并采用首个真条件优先语义。

### `src/backend/beir.cpp`
- BEIR text dump、validation、operation/type helpers、mutable builder 实现；Ite/Case 位值分析按结果位宽合并所有可达分支的已知位，常量条件可裁掉不可达结果并继续常量化。

### `src/backend/beopt.hpp`
- 声明 BEIR optimizer options、可选逐轮回调和 `optimizeProgram`。

### `src/backend/beopt.cpp`
- 串接 BEIR optimization passes，并在固定点循环每轮开始时触发可选逐轮回调。
- 布尔归一化参与主固定点和谓词下沉后的受限清理；随后单向执行互斥 mux 到 Case 的展平、再次布尔归一化及结合树平衡，并对新结构重新执行常量、代数、位宽、Assign、CSE、DCE 清理。
- `Options` 暴露 max_predicate_iterations、max_predicate_formulas、max_predicate_atoms、max_mux_branches、max_tree_leaves、max_bit_range_updates、max_bit_compose_pieces；谓词证明默认每次查询最多使用 16384 个可达公式、512 个原子。`--beopt mux/no-mux`、`balance/no-balance`、`bit-updates/no-bit-updates` 与 `boolean/no-boolean` 分别控制结构 pass，all/none 同时管理它们。

### `src/backend/beopt_constant.hpp`
- 常量传播、常量折叠和 literal 简化。

### `src/backend/beopt_algebraic.hpp`
- 代数化简；支持 Ite 的常量条件和等值分支折叠；Case 会删除恒假分支、在恒真分支处截断后续分支，并折叠全部结果相同的选择。

### `src/backend/beopt_boolean.hpp`
- 输入关系证明后保存内存快照；AIG 优化完成、真值表之前单独拓扑重建一轮，按稳定输入标识重传播蕴含/互斥关系并化简，不重复证明。
- Boolean DAG 构建后证明输入之间的互斥、蕴含、等价/互补关系：先用比较运算符规则证明常量 Eq/Ne 等关系，一般输入对不设对数上限，按三级驱动锥公共信号索引筛选，并使用叶子比较约束的局部布尔枚举证明；支持共享操作数的六种比较、交换操作数及无符号常量阈值关系。关系随重建共享，用于 AND/OR 常量化、冗余条件删除及等价输入合并；未知结果不改写。预算及 FPU 验证见 [`testv2/bool_input_relations.md`](testv2/bool_input_relations.md)。
- 局部代数扫描后、真值表前执行最多 3 轮 AIG 重建和平衡：AND/补边统一表示常量、反相及 OR；拓扑重建传播常量和合并相同表达式。仅展开单扇出、非输出边界区域（深度最多 8、叶子最多 64），按到达深度优先合并，仅接受严格降深的平衡改写。
- DAG 导入后、真值表前增加最多 4 轮拓扑扫描，检查深度 3 驱动锥的幂等、互补及吸收模式，无变化提前退出；复用常量/互补项/MUX 规则，XOR 统一输入极性并化简互补数据臂。补边直接实现双重否定、德摩根归一化，避免双向展开。
- 所有标量一位信号统一导入多输入、多输出 AND/补边 DAG；Assign、NOT、AND/OR/XOR、一位 Eq/Ne、ITE 和合法 Case 导入为内部逻辑，Case 保留首真优先语义。多位操作产生的一位结果及不支持的操作作为不透明输入，外部消费者和可观察端口标为输出。
- 图内执行常量传播、规范化结构共享及有界局部代数化简；单扇出且不跨输出边界的局部区域进行精确真值表改写，最多 16 输入、8 层、256 个内部节点，全 pass 真值表分析预算为 268435456 个行/节点工作单位，仅接受 AND 节点数下降的替换。
- 统一重建各输出并再次合并共享结构，只发射可达节点；尽量复用原有信号避免重复运行不断生成临时节点。pass 自带 BEIR DCE，不依赖外部 CSE/Constant/DCE 开关。实现及边界约束见 [`boolnorm.md`](boolnorm.md)。

### `src/backend/beopt_assign_chains.hpp`
- assignment chain 简化。

### `src/backend/beopt_cse.hpp`
- common subexpression elimination。

### `src/backend/beopt_dce.hpp`
- dead code elimination。

### `src/backend/beopt_predicate.hpp`
- predicate/guard 相关 BEIR 优化。
- 当前下沉流程、预算覆盖范围及静态性能风险见 [`sinkpred.md`](sinkpred.md)。
- `PredicateRelations::implies/isExclusive` 对调用者统一返回 Proven/Unknown；详细查询另区分 Proven、Counterexample 和 ResourceLimit。SAT/DPLL 证明按单次查询实际可达的公式和原子数限制资源，超限只使该查询返回 Unknown，不停止后续节点或更小子树的证明。
- 下沉从候选 Ite 沿消费者遍历至首个 Ite 数据臂，逐使用位置收集启用条件，要求全部条件蕴含候选 guard 的同一极性。直接输出、条件端口、Case/Call 边界保守放弃；候选及消费者边遍历预算在分析期间生效，超限不授权改写。按驱动图逆拓扑顺序处理 Ite，成功后立即提交 Assign、更新 use 边并清空证明缓存，使上游候选能穿过已消除的下游选择；不再调用全路径 demand-context/support 分析。

### `src/backend/beopt_width.hpp`
- width 相关优化和裁剪；支持 operand signed view 影响的扩展语义；Case 的结果宽度取所有分支值与默认值的合并需求，条件保持一位，结果需求反向传播到每个值分支。

### `src/backend/rtlgen.hpp`
- 声明 SystemVerilog emitter。

### `src/backend/rtlgen.cpp`
- 将 BEIR program emit 为 synthesizable SystemVerilog。
- 乘法分别按左右操作数的 signed view 扩展/截断到结果位宽，再以无符号位模式相乘并显式截断，保证混合符号语义不依赖 BEOPT。
- 支持 scalar/array ports、BEIR lookup、assign/operation lowering。
- 原生 BEIR Case 输出为带完整 default 的 `always_comb unique case (1'b1)`，向 RTL 工具声明分支条件互斥；省略位宽适配后的赋值表达式与 default 完全相同的分支。

### `src/backend/circt_bridge.hpp` / `src/backend/circt_bridge.cpp`
- 将 BEOPT 后的纯组合 BEIR lower 为 CIRCT HW/Comb IR，调用 `circt-opt --canonicalize --cse --hw-cleanup --lower-hw-to-sv --export-verilog`，并提取导出的 Verilog module 或 module body。
- 支持标量组合运算、首真优先 Case、Aggregate-backed signal Lookup、动态位/片选择和动态位/片写入；动态操作被 lower 为明确的组合选择网络。
- `--circt` / `CompileOptions::use_circt` 显式启用。CIRCT 缺失、IR 不支持或优化失败均为编译错误，不回退 native emitter；非 release 调用保留输入 MLIR、导出输出和 stderr 以供诊断。
- RTL emission 将全常量 Aggregate 的 Lookup/ArrayAccess 输出为组合 case 查表函数；按元素类型、表长及规范化常量位值建立哈希索引，以完整比较处理哈希冲突，同一模块内相同表复用函数；可追踪赋值及无符号宽度转换链，省略不再被使用的数组，运行时表项保持数组读取。

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
- End-to-end integer/fixint API fixture：arithmetic、bit op、shift、compare、slice、pick、cat/repeat/reduce、cast、enum、standard integer mixing；乘法覆盖左右混合符号、双有符号、不同位宽和结果截断。
- 合并动态位段/单 bit 写入顺序、RHS/LHS 自增求值顺序、128 位常量循环、运行时数据静态切片、`i * 4` 重叠写入、首尾及整宽覆盖回归。

### `testv2/fixtures/constant_rom.logic.cpp`
- AES SBOX 常量 ROM fixture，覆盖两个动态索引读取；用于 case 查表的 C++/RTL 差分验证。

### `testv2/fixtures/flatten_misc.logic.cpp`
- End-to-end struct/array/aggregate/dynamic index/constant lookup fixture。

### `testv2/fixtures/controlflow_misc.logic.cpp`
- End-to-end nested if/switch/loop、dynamic break/continue、early-return fixture。

### `testv2/fixtures/inline_misc.logic.cpp`
- End-to-end helper/lambda inline、overload、parameter/return fixture。

### `testv2/fixtures/port_lift_large_array.logic.cpp`
- End-to-end 32×64-bit array global-port lift、嵌套 helper const-ref alias、RTL/C++ 差分 fixture；防止 helper 内联为每个读取调用复制完整数组。

### `testv2/fixtures/s*/...`
- Stage-specific C++ fixtures used by integration tests.

## Scripts

### `testv2/regression.sh`
- 一键递归运行 `testv2/fixtures/**/*.logic.cpp` 的 C++/RTL 随机差分；每个 fixture 分别使用 native emitter 与 `--circt` CIRCT emitter 验证。自动构建 `predicate-expand`，从 `testv2/regression.json` 读取输入域约束，区分 `PASS`、预期负向测试 `XFAIL`、`FAIL` 与 `XPASS`，并在逐项日志和 `summary.tsv` 中记录后端。

### `testv2/regression.json`
- 全量差分的 fixture 配置清单；`input_ranges` 以 raw port value 的闭区间 `[MIN, MAX]` 限制随机输入，避免 C++ oracle 执行 fixture 前置条件之外的未定义行为。

### `scripts/differential_rtl.py`
- V2 RTL differential harness。
- `--circt` 仅让 RTL 生成阶段走 CIRCT；端口元数据仍来自同一 BEIR 程序，因此 C++ oracle、随机输入和比较规则与 native 后端完全相同。
- 生成 port metadata、RTL、C++ oracle 和 Verilator testbench；oracle 直接写入源文件全局 input port、调用无参 top、读取全局 output port，并比较随机输入下的输出。
- `--input-config` 加载 fixture manifest，重复的 `--input-range NAME=MIN:MAX` 可覆盖单个输入范围；配置会校验输入名、闭区间顺序和端口位宽。oracle 异常退出会报告 case、signal/exit code 与完整触发输入。

## Third Party And Docs

### `third_party/vulsim/vullib/fixint.hpp`
- VUL fixed-width `Int<N>` runtime header used by source fixtures and C++ oracle。

### `third_party/vulsim/vullib/common.h`
- VUL common integer aliases and helpers。

### `docs/THIRD_PARTY_NOTICES.md`
- Third-party notices。

### `roadmap.md`
- V2 pipeline stage responsibilities and long-term compiler route。

### `scripts/check_unrolled_pick.py`
- 针对 `int_misc.logic.cpp` 的 `pick_*` 输出逐个遍历 BEIR 依赖，检查静态化、运行时索引对照及 RTL 中的 128 位常量折叠，再执行完整 int_misc 的 100 组 C++/RTL 差分。
- `python3 scripts/check_unrolled_pick.py --build-dir <build>`；任一结构或数值检查失败均返回非零。

### `src/backend/beopt_slices.hpp`
- 常量传播后，将范围合法的常量索引 DynamicSlice/DynamicWriteSlice 静态化；静态 WriteSlice 保留范围元数据，交给 bit-range update pass 收集整条更新链。
- 由 width 优化开关控制；未知或越界索引保留原操作。

### `src/backend/beopt_bit_updates.hpp`
- `BitRangeUpdateCoalescingPass` 从输出侧收集单活跃用户的静态 WriteSlice 链，穿透同类型 Assign 别名，并按位区间生成一次扁平 Assign/Concat。
- 重叠区间使用最新写入值，空洞保留原值；共享中间结果、类型或范围不匹配时保守停止。默认最多收集 1024 次更新和 2048 个组合片段，硬上限分别为 4096 和 8192。
- 用户计数仅覆盖可观察输出可达图，避免已折叠、尚未 DCE 的死别名阻止合并；变换后使 value facts 失效。

### `src/backend/beopt_case_guards.hpp`
- 在主优化固定点之后，对 Case 条件建立带补边的共享布尔 DAG，并在保留的路径事实下进行 cofactor 化简；删除条件时不使用该条件本身作为证明前提。
- 将最多 16 位的常量相等/不等译码展开成精确位约束，穿透安全的 Assign/Slice/BitSelect/Trunc/扩展边界，使不同状态译码能裁掉包含数据计算的前序排除条件；其他数据运算保守作为原子；完整位译码在输出时重新合并为单个等值比较。
- 对原生有序 Case 使用原始条件构建平衡前缀 OR，显式排除更早命中的分支；对已经互斥的分支，通过路径化简消除冗余屏蔽。负向路径事实也重建为共享的平衡 OR 前缀加取反。
- 化简保持精确语义及分支互斥，采用有界结构证明（不承诺任意布尔函数的全局最小式）；最终只做代数、CSE、Assign、DCE 清理，避免重新串行化控制前缀。

### `src/backend/beopt_structure.hpp`
- `parallelizeExclusiveMuxes` 遍历等宽 Ite 的 true/false 两臂，收集完整路径谓词并转换为一个有序 Case，保留显式默认值。叶路径在分叉处包含相反条件，结构性保证互斥并保留原优先级语义；共享节点和 signed-view 边界保持为叶子。默认最多 1024 个分叉（硬上限 4096），超限候选不修改。
- `balanceAssociativeTrees` 支持等宽无 signed-view 的 AND/OR/XOR/模加法；默认最多 1024 叶（硬上限 4096），只展开单用户同类节点，不穿越截断/扩展。按到达时间优先合并早到输入，且仅在估计延迟严格下降时重构。
- 两个 pass 按可观察输出遍历活跃图，修改后使 value facts 失效。

### `testv2/beopt_structure_test.cpp`
- 独立 BEIR 求值器验证 mux 到 Case 的默认值和等价性、非互斥拒绝、8 输入结合树及模加法语义；覆盖完整父上下文、查询失效、共享节点、位宽边界、晚到输入、下沉后再次简化及选项解析。

### `testv2/beir_case_test.cpp`
- 构造原生 Case 验证首真分支布局、共同已知位和值事实、常量传播、恒真/恒假和同值代数折叠、位宽需求传播、谓词分支上下文、CSE/DCE、BEIR 文本及 `always_comb case` RTL 发射。

### `testv2/beopt_boolean_test.cpp`
- 穷举验证短路 phi、吸收律、consensus、一般 ITE 和优先级 Case；覆盖多输出共享、死逻辑清理、多位输入边界及连续调用稳定性。400 个随机多输出图各穷举 16 组输入，覆盖 XOR/Eq/Ne/ITE/Case。

### `testv2/fixtures/backend_structure.logic.cpp`
- 8/128 位互斥选择、优先级选择、结合运算及嵌套谓词的端到端 C++/RTL 差分回归。

### `testv2/fixtures/branch_decision.logic.cpp`
- 软件风格控制转移判断 fixture：外层 legal/jal/jalr/branch 条件、六路 if/else-if 分支比较、布尔变量反复赋值，以及 JALR/PC target 选择；覆盖有符号和无符号 64 位比较。

### `testv2/branch_decision_analysis.md`
- 记录该 fixture 的语义差分和结构深度结果：完整路径 guard 可证明互斥，taken 的六级串行更新被一个原生 Case 替代；布尔归一化继续移除一位短路/phi Ite，使 control_valid 与 taken 的 Ite 深度降为 0。

### `testv2/beopt_bit_updates_test.cpp`
- 独立 BEIR 求值器验证相邻、重叠、稀疏和全覆盖写入；覆盖 Assign 别名穿透、共享中间节点边界、规模限制及选项解析。

### `testv2/fixtures/bit_update_coalescing.logic.cpp`
- 相邻、重叠和带空洞静态位段更新的端到端 C++/RTL 差分 fixture。

### `testv2/case_guard_analysis.md`
- 记录 Case 路径消冗余、128 路优先级前缀、随机布尔穷举与 FPUArithmetic 实际生成结果；FMA_SUM1 条件恢复为单个状态比较，并注明图深度统计方法和验证范围。
