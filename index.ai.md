# src 代码索引

## 统一回归测试流程

后续修改代码后，优先使用以下流程确认 `tests/fixtures` 正向用例的 RTL 生成与随机差分测试仍然正确。

1. 配置构建目录：

```bash
cmake -S . -B build
```

2. 构建前端/后端编译器：

```bash
cmake --build build --target predicate-expand -j2
```

3. 对 `tests/fixtures` 执行全量 RTL diff 回归：

```bash
for f in tests/fixtures/*.logic.cpp; do
  echo "== $f"
  python3 scripts/differential_rtl.py "$f" --top hls_main --cases 50 || exit 1
done
```

4. 对高风险修改加大随机样本数复测代表用例：

```bash
python3 scripts/differential_rtl.py tests/fixtures/int_range.logic.cpp --top hls_main --cases 200
python3 scripts/differential_rtl.py tests/fixtures/int_uint_corner.logic.cpp --top hls_main --cases 200
python3 scripts/differential_rtl.py tests/fixtures/array_flatten.logic.cpp --top hls_main --cases 200
```

5. 排查优化相关问题时，可关闭 BEIR 优化对比：

```bash
python3 scripts/differential_rtl.py tests/fixtures/int_range.logic.cpp --top hls_main --cases 100 --beopt none
```

说明：`differential_rtl.py` 会调用 `build/predicate-expand` 生成 listjson 与 RTL，使用 `third_party/vulsim/vullib` 作为 `--vullib`，再通过 Verilator 与 C++ oracle 做随机输入差分。

## 顶层 API

### `src/rtlzz.hpp`
- 主要功能：提供 header-only 编译 API，将 C++ 源码行编译为 RTL、listjson 或 BEIR。
- 主要函数：
  - `outputKindName`：返回内部输出格式枚举的文本名称。
  - `buildClangArgs`：根据 `CompileOptions` 组装传给 clang 的参数。
  - `joinCodeLines`：将输入源码行拼接为完整源码文本。
  - `splitCodeLines`：将输出文本拆回源码行数组。
  - `buildPredicateProgram`：把 normalize/SSA 结果组装成 `PredicateProgram`。
  - `compileSource`：执行 parse、unroll、inline、normalize、CFG、SSA、predicate、emit 全流程。
  - `compileToRtl`：编译输入源码并输出 SystemVerilog RTL。
  - `compileToListJson`：编译输入源码并输出 listjson。
  - `compileToBeir`：编译输入源码并输出 BEIR 文本。

## AST 前端

### `src/ast/AST.h`
- 主要功能：定义前端 AST 的类型、表达式、语句、函数和便捷构造函数。
- 主要函数：
  - `firstDebugLoc`：从子表达式选择第一个有效源码位置，让 lowering/展开生成的复合表达式继承源位置。
  - `is_bool_type_info`：判断 `TypeInfo` 是否表示布尔类型。
  - `canonical_bool_type`：构造规范布尔类型。
  - `canonicalize_bool_type`：将布尔等价类型规整为规范布尔类型。
  - `paramDirectionName`：返回参数方向枚举的文本名称。
  - `make_literal`：构造字面量表达式。
  - `make_var`：构造变量表达式。
  - `make_binary`：构造二元表达式。
  - `make_unary`：构造一元表达式。
  - `make_ternary`：构造三元选择表达式。
  - `make_array_access`：构造数组访问表达式。
  - `make_field_access`：构造字段访问表达式。
  - `make_hw_type`：构造硬件整数类型。
  - `make_bool_type`：构造布尔类型。
  - `make_bits_type`：构造指定位宽的位向量类型。
  - `make_unknown_type`：构造未知类型占位。
  - `make_zext`：构造零扩展表达式。
  - `make_sext`：构造符号扩展表达式。
  - `make_trunc`：构造截断表达式。
  - `make_slice`：构造静态切片表达式。
  - `make_bit_select`：构造静态位选择表达式。
  - `make_write_slice`：构造静态切片写表达式。
  - `make_write_bit`：构造静态位写表达式。
  - `make_dynamic_write_slice`：构造动态定宽切片写表达式。
  - `make_dynamic_write_bit`：构造动态位写表达式。
  - `make_concat`：构造拼接表达式。
  - `make_repeat`：构造重复表达式。
  - `make_reduce`：构造归约表达式。

### `src/ast/ASTBuilder.h`
- 主要功能：声明基于 libclang 构建前端 AST 的入口。
- 主要函数：
  - `buildASTFromSource`：从源码文件解析并抽取目标函数 AST。
  - `buildASTFromSourceText`：从内存源码文本解析并抽取目标函数 AST。

### `src/ast/ASTBuilder.cpp`
- 主要功能：保留 ASTBuilder 编译单元，目前具体实现位于 driver 文件。
- 主要函数：无独立主要函数。

### `src/ast/ASTBuilderDriver.cpp`
- 主要功能：使用 libclang 将 C++ 子集转换为项目 AST，并收集函数、lambda、结构体和常量元数据。
- 主要函数：
  - `buildASTFromSource`：文件输入形式的 AST 构建入口。
  - `buildASTFromSourceText`：文本输入形式的 AST 构建入口。
  - `buildASTFromSourceImpl`：统一执行 clang 解析、目标函数匹配、元数据收集和 AST 转换。
  - `convertFunctionDecl`：将 clang 函数声明转换为 `FunctionAST`。
  - `convertLambdaExpr`：将 lambda 表达式转换为可 inline 的函数 AST。
  - `convertExpr`/`convertExprImpl`：将 clang 表达式游标转换为 AST 表达式。
  - `convertStmt`/`convertStmtImpl`：将 clang 语句游标转换为 AST 语句。
  - `convertBlock`：转换复合语句块。
  - `convertType`：将 clang 类型转换为 `TypeInfo`。
  - `inferParamDirection`：根据 const 值/引用和非 const 引用推断端口方向。
  - `invalidTopParamReason`：诊断不合法的目标函数参数形式。
  - `collectSourceFunctionDefinitions`：收集源文件中的函数定义。
  - `checkHelperCallGraphRecursion`：检查 helper 调用图递归。
  - `registerStructMetadata`：登记结构体字段和构造函数信息。
  - `collectStructFieldLayouts`：收集结构体字段布局。
  - `collectGlobalConstInts`：收集全局常量整数。
  - `collectLocalLambdas`：收集目标函数内部 lambda。

### `src/ast/VulTypeRecognizer.h`
- 主要功能：声明 VUL/fixint、内建整数、结构体和 `std::array` 类型识别接口。
- 主要函数：
  - `recognizeHwIntType`：识别硬件整数类型。
  - `recognizeBuiltinFixedWidth`：识别内建固定位宽整数类型。
  - `recognizeRecordType`：识别记录/结构体类型。
  - `recognizeStdArrayType`：识别 `std::array` 类型。

### `src/ast/VulTypeRecognizer.cpp`
- 主要功能：实现 clang 类型到 `TypeInfo` 的识别和模板参数解析。
- 主要函数：
  - `recognizeHwIntType`：识别 `UInt`/`Int` 等硬件整数模板类型。
  - `recognizeStdArrayType`：解析 `std::array<T, N>` 的元素类型和维度。
  - `recognizeBuiltinFixedWidth`：识别 `uint*_t`、`int*_t`、`bool` 等内建类型。
  - `recognizeRecordType`：识别普通 record 类型并记录结构体名称。

### `src/ast/VulOpRecognizer.h`
- 主要功能：声明 fixint/VUL API 调用的分类识别接口。
- 主要函数：
  - `recognizeTemplateInt`：读取调用或类型上的整数模板参数。
  - `recognizeVulCall`：识别特定 fixint/VUL 调用种类和参数。

### `src/ast/VulOpRecognizer.cpp`
- 主要功能：实现 fixint/VUL 方法、全局函数和模板参数的识别。
- 主要函数：
  - `recognizeTemplateInt`：从 clang cursor 中提取指定模板整数。
  - `recognizeVulCall`：将调用识别为 slice、bit、cat、repeat、reduce、signed view 等语义。

## Pipeline

### `src/pipeline/Stages.h`
- 主要功能：声明 parse、unroll、inline、normalize、CFG、SSA 六个显式阶段接口。
- 主要函数：
  - `parseSource`：解析源码得到目标函数 AST。
  - `unrollFunction`：对函数体执行循环展开。
  - `inlineHelpersAndLambdas`：执行 helper/lambda inline 阶段。
  - `normalizeFunction`：执行 normalize、flatten 和 lowering。
  - `buildControlFlow`：构建控制流图。
  - `buildSSAForm`：构建 SSA 表示。

### `src/pipeline/Stages.cpp`
- 主要功能：实现各 pipeline 阶段的薄封装和错误隔离。
- 主要函数：
  - `parseSource`：选择文件或文本输入并调用 AST builder。
  - `unrollFunction`：调用 loop unroll 并更新函数体。
  - `inlineHelpersAndLambdas`：调用 inline pass 并更新函数体。
  - `normalizeFunction`：调用 normalize pass。
  - `buildControlFlow`：从 normalize 后语句生成 CFG。
  - `buildSSAForm`：从 CFG 和 seed 符号生成 SSA。

## Normalize 与 Lowering

### `src/transform/Normalize.h`
- 主要功能：定义 normalize 阶段结果，并声明 inline 与 normalize 入口。
- 主要函数：
  - `inlineHelpersAndLambdas`：在展平前展开 helper 和 lambda。
  - `normalizeFunction`：检查 C++ 子集并降低为 predicate 友好的扁平表达式。

### `src/transform/Normalize.cpp`
- 主要功能：保留 transform normalize 编译单元，实际实现分布在 `normalize` 目录。
- 主要函数：无独立主要函数。

### `src/normalize/NormalizeUtils.h`
- 主要功能：集中声明 normalize 子模块共享的类型、表达式、inline、数组和别名工具函数。
- 主要函数：
  - `isUIntName`：判断类型名是否为无符号整数名。
  - `explicitHwWidthFromName`：从显式硬件整数名解析位宽。
  - `isMutableParam`：判断参数是否可写。
  - `isOutputParam`：判断参数是否为输出。
  - `isInputParam`：判断参数是否为输入。
  - `baseName`：提取表达式基础变量名。
  - `cloneExpr`：深拷贝表达式。
  - `castIfWidthChanges`：在位宽变化时插入类型转换。
  - `normalizeConstantOperandsForBinary`：规范二元表达式中的常量操作数位宽。
  - `substituteInlineExpr`/`substituteInlineStmt`/`substituteInlineStmts`：执行 inline 替换。
  - `collectVarRefs`/`collectStmtVarRefs`：收集表达式或语句中的变量引用顺序。
  - `guardStmt`：给语句添加条件守卫。
  - `localizeProcedureReturns`：将过程式 return 局部化为可 inline 语句。
  - `makeAssignStmt`：构造赋值语句。
  - `makeLookupExpr`：构造 lookup 表达式。
  - `collectArrayAccess`：拆解多维数组访问。

### `src/normalize/NormalizeDriver.cpp`
- 主要功能：执行 normalize 主流程，包括端口规则检查、结构体/数组展平、Int API lowering、默认值补全和赋值重写。
- 主要函数：
  - `normalizeFunction`：normalize 阶段入口。
  - `rewriteExpr`：递归重写表达式并执行类型/数组/结构体/API lowering。
  - `rewriteStmt`：递归重写语句并处理声明、赋值、分支、switch、return 等结构。
  - `rewriteStmts`：重写语句序列并维护 normalize 环境。
  - `rewriteTarget`：重写赋值左值。
  - `rewriteArrayAccess`：降低数组访问为扁平元素、lookup 或 mux。
  - `rewriteArrayAssign`：展开数组元素写入。
  - `rewriteWholeArrayAssign`：展开整数组赋值。
  - `rewriteBitSliceAssign`：降低 bit/slice 作为左值的写操作。
  - `inlineHelperCall`：降低可表达式化的 helper 调用。
  - `inlineProcedureCall`：展开过程式 helper 调用。
  - `lowerProxyMethodExpr`：降低特殊 proxy 方法表达式。
  - `lowerProxyProcedureCall`：降低特殊 proxy 过程调用。
  - `buildPackedStructValue`：将结构体值打包为扁平位向量。
  - `buildPackedArrayValue`：将数组值打包为扁平位向量。
  - `flattenedTypeWidth`：计算结构体/数组展平后的总位宽。
  - `appendStructUnpackDecls`：生成结构体字段解包声明。
  - `addStructFieldSymbols`：登记结构体展平字段符号。
  - `addFlattenedArraySymbols`：登记数组展平元素符号。
  - `checkStructOutputInitialized`：检查结构体输出字段是否完全赋值。

### `src/normalize/FunctionInline.cpp`
- 主要功能：实现 helper 函数 inline 的表达式替换、语句替换和控制流守卫合成。
- 主要函数：
  - `inlineHelpersAndLambdas`：对函数体中的 helper/lambda 调用执行 inline。
  - `substituteInlineExpr`：替换 inline 表达式中的形参和局部名。
  - `substituteInlineStmt`：替换 inline 语句中的形参和局部名。
  - `substituteInlineStmts`：批量替换 inline 语句。
  - `collectVarRefs`：收集表达式变量引用。
  - `collectStmtVarRefs`：收集语句变量引用。
  - `notExpr`：构造逻辑非表达式。
  - `andExpr`：构造逻辑与表达式。
  - `guardStmt`：为语句套上条件守卫。
  - `containsReturnStmt`：判断语句序列是否包含 return。

### `src/normalize/LambdaInline.cpp`
- 主要功能：将 inline 后的局部 return 语义转换为显式返回值和存活条件。
- 主要函数：
  - `localizeProcedureReturns`：将过程式 return 改写为局部返回变量和 alive guard。
  - `localizeReturnSeq`：处理语句序列中的 return 合并。
  - `mergeAliveAfterIf`：合并 if 两侧分支后的存活条件。

### `src/normalize/IntOpLowering.cpp`
- 主要功能：实现硬件整数和常量位宽语义的 normalize 辅助逻辑。
- 主要函数：
  - `isUIntName`：判断整数类型名是否无符号。
  - `explicitHwWidthFromName`：解析硬件整数类型名中的显式位宽。
  - `isInputParam`/`isOutputParam`/`isMutableParam`：判断参数方向和可变性。
  - `baseName`：提取表达式基础名。
  - `isConstantExpr`：判断表达式是否为常量。
  - `isWidthCastableConstantExpr`：判断常量是否可按目标位宽转换。
  - `resultTypeForBinary`：推断二元运算结果类型。
  - `castIfWidthChanges`：按目标类型插入扩展或截断。
  - `normalizeConstantOperandsForBinary`：统一二元常量操作数类型。
  - `normalizeConstantBranchesForTernary`：统一三元分支常量类型。
  - `cloneExpr`：深拷贝表达式。
  - `literalIntValue`：读取整数字面量值。
  - `isSignedViewExpr`：判断表达式是否带 signed view。
  - `foldConstantOvershift`：折叠常量过量移位。

### `src/normalize/AliasLowering.cpp`
- 主要功能：提供别名和字段访问路径识别工具。
- 主要函数：
  - `directVarName`：识别直接变量名。
  - `fieldAccessPath`：拆解字段访问为对象名和字段路径。

### `src/normalize/ArrayScalarize.cpp`
- 主要功能：提供数组访问拆解、元素类型提取和扁平索引命名工具。
- 主要函数：
  - `collectArrayAccess`：收集数组基址和多级索引。
  - `scalarTypeFromArray`：取得数组最终元素类型。
  - `joinIndexName`：生成扁平数组元素名。
  - `flatElementCount`：计算数组展平元素数量。
  - `targetName`：提取赋值目标名。
  - `literalIndex`：读取字面量数组索引。

### `src/normalize/StructFlatten.cpp`
- 主要功能：提供结构体名称规范化工具。
- 主要函数：
  - `canonicalStructName`：规整结构体类型名。

### `src/normalize/AssignmentLowering.cpp`
- 主要功能：提供赋值语句构造工具。
- 主要函数：
  - `makeAssignStmt`：构造 AST 赋值语句。

### `src/normalize/LookupLowering.cpp`
- 主要功能：提供 lookup 表达式构造工具。
- 主要函数：
  - `makeLookupExpr`：构造 lookup 调用表达式并携带表类型。

### `src/normalize/DefaultTotalizationPass.cpp`
- 主要功能：提供 normalize 默认补全模块链接占位。
- 主要函数：
  - `defaultTotalizationModuleLinked`：返回 normalize 默认补全模块是否链接。

## 循环、CFG、SSA 与 Predicate

### `src/transform/LoopUnroll.h`
- 主要功能：声明静态循环展开配置、结果和入口。
- 主要函数：
  - `unrollLoops`：对语句体执行静态循环展开。

### `src/transform/LoopUnroll.cpp`
- 主要功能：实现 for/while 静态循环展开和 break/continue 降低。
- 主要函数：
  - `unrollLoops`：循环展开入口。
  - `extractForParams`：从 for 语句提取循环参数。
  - `extractWhileParams`：从 while 语句和前置赋值提取循环参数。
  - `computeIterations`：计算静态循环迭代次数。
  - `substituteVar`：用常量迭代值替换表达式变量。
  - `substituteStmt`/`substituteStmts`：替换语句中的迭代变量。
  - `lowerLoopControl`：将 break/continue 降低为显式控制条件。
  - `processStmts`：递归处理语句序列中的循环。

### `src/ir/CFG.h`
- 主要功能：定义控制流图基本块、终结符和构建入口。
- 主要函数：
  - `buildCFG`：从 normalize 后语句体构建 CFG。

### `src/ir/CFG.cpp`
- 主要功能：实现结构化语句到 CFG 的转换。
- 主要函数：
  - `CFG::newBlock`：创建基本块。
  - `CFG::getBlock`：按块 ID 获取基本块。
  - `CFG::addEdge`：添加 CFG 边。
  - `CFGBuilder::buildStmt`：将单条语句加入 CFG。
  - `CFGBuilder::buildStmts`：将语句序列加入 CFG。
  - `buildCFG`：CFG 构建入口。

### `src/ir/SSA.h`
- 主要功能：定义 SSA 变量、phi、SSA 基本块、SSA 程序和构建入口。
- 主要函数：
  - `buildSSA`：从 CFG 和 seed 符号构建 SSA。

### `src/ir/SSA.cpp`
- 主要功能：实现支配关系、phi 插入和变量重命名。
- 主要函数：
  - `buildSSA`：SSA 构建入口。
  - `computeDominators`：计算 CFG 支配信息。
  - `collectDefs`：收集变量定义点。
  - `renameExprUses`：重命名表达式中的变量使用。
  - `renameExprDef`：重命名表达式中的变量定义。

### `src/transform/Predicate.h`
- 主要功能：声明 SSA 到 predicate IR 的转换入口。
- 主要函数：
  - `predicate`：将 SSA 程序转换为带 guard 的 predicate 程序。

### `src/transform/Predicate.cpp`
- 主要功能：将 SSA 块条件和 phi 转换为 guarded assignment 与 ITE 表达式。
- 主要函数：
  - `predicate`：predicate 转换入口。
  - `computeBlockGuards`：计算每个 SSA 基本块的进入条件。
  - `phiToIte`：将 phi 节点转换为 ITE 表达式。
  - `phiIncomingGuard`：计算 phi 输入对应的分支 guard。
  - `isTautology`：判断布尔 guard 是否恒真。

### `src/predicate/PredicateIR.h`
- 主要功能：定义 predicate IR、guarded assignment、输出表达式和 guard 构造函数。
- 主要函数：
  - `make_ite`：构造条件选择表达式。
  - `make_and`：构造 guard 逻辑与。
  - `make_not`：构造 guard 逻辑非。
  - `make_true_guard`：构造恒真 guard。

### `src/predicate/PredicateVerifier.h`
- 主要功能：声明 predicate IR 校验入口。
- 主要函数：
  - `verifyPredicateProgram`：校验 predicate 程序合法性。

### `src/predicate/PredicateVerifier.cpp`
- 主要功能：检查 predicate 程序的闭包、端口方向、SSA 使用和非法表达式。
- 主要函数：
  - `verifyPredicateProgram`：校验入口。
  - `verifyExpr`：校验普通表达式。
  - `verifyClosedExpr`：校验表达式只引用允许符号。
  - `exprContainsVar`：检测表达式是否引用指定变量。
  - `directionForBaseName`：查询基础名的端口方向。

### `src/predicate/OutputExpressionMap.h`
- 主要功能：声明输出表达式合成入口。
- 主要函数：
  - `buildOutputExpressionMap`：从 guarded assignment 合成每个输出的完整表达式。

### `src/predicate/OutputExpressionMap.cpp`
- 主要功能：将多 guard 赋值合成为完整输出表达式，并执行局部布尔/覆盖简化。
- 主要函数：
  - `buildOutputExpressionMap`：输出表达式合成入口。
  - `simplifyOutputExpr`：递归简化输出表达式。
  - `mergeWithGuard`：按 guard 合并赋值值。
  - `controlExprForOutput`：为输出寻找配对控制信号表达式。
  - `coverageForOutput`：计算输出赋值覆盖信息。
  - `defaultValueFor`：为类型生成默认值。

### `src/predicate/DefaultTotalizationPass.h`
- 主要功能：声明输出默认补全分类和默认值构造接口。
- 主要函数：
  - `classifyDefaultTotalization`：判断某个输出是否需要语义默认值。
  - `makeDefaultTotalizedValue`：构造默认补全表达式。

### `src/predicate/DefaultTotalizationPass.cpp`
- 主要功能：实现输出默认值策略。
- 主要函数：
  - `classifyDefaultTotalization`：按输出名、类型和元数据分类默认补全策略。
  - `makeDefaultTotalizedValue`：生成对应类型的默认补充值。

## 通用 IR 与求值

### `src/ir/IRType.h`
- 主要功能：定义早期 typed DAG 使用的 IR 类型描述。
- 主要函数：无独立主要函数。

### `src/ir/IRValue.h`
- 主要功能：定义 typed DAG 值引用。
- 主要函数：无独立主要函数。

### `src/ir/IRLValue.h`
- 主要功能：定义 IR 左值路径描述。
- 主要函数：无独立主要函数。

### `src/ir/IRNode.h`
- 主要功能：定义早期 IR 节点和源位置结构。
- 主要函数：无独立主要函数。

### `src/ir/TypedDAG.h`
- 主要功能：声明带类型 DAG 节点构造、去重和序列化接口。
- 主要函数：
  - `makeNode`：创建或复用 DAG 节点。
  - `makeVar`：创建变量节点。
  - `makeLiteral`：创建字面量节点。
  - `hasCycle`：检测 DAG 是否存在环。
  - `stableJson`：输出稳定 JSON 表示。

### `src/ir/TypedDAG.cpp`
- 主要功能：实现 typed DAG 的节点去重、循环检测和稳定 JSON 输出。
- 主要函数：
  - `IRType::key`：生成类型稳定键。
  - `TypedDAG::makeNode`：创建或复用节点。
  - `TypedDAG::makeVar`：创建变量节点。
  - `TypedDAG::makeLiteral`：创建字面量节点。
  - `TypedDAG::hasCycle`：检测依赖环。
  - `TypedDAG::stableJson`：输出稳定 JSON。
  - `TypedDAG::keyFor`：生成节点去重键。

### `src/ir/CanonicalIR.h`
- 主要功能：声明规范类型、规范操作和类型推断接口。
- 主要函数：
  - `canonicalTypeFromTypeInfo`：从 `TypeInfo` 构造规范类型。
  - `typeInfoFromCanonical`：从规范类型还原 `TypeInfo`。
  - `canonicalOpName`：返回规范操作名。
  - `canonicalCast`：推断 cast 规范操作。
  - `canonicalBinary`：推断二元规范操作。
  - `canonicalUnary`：推断一元规范操作。
  - `canonicalSlice`：推断静态切片操作。
  - `canonicalBitSelect`：推断静态位选择操作。
  - `canonicalWriteSlice`：推断切片写操作。
  - `canonicalWriteBit`：推断位写操作。
  - `canonicalConcat`：推断拼接操作。
  - `canonicalRepeat`：推断重复操作。
  - `canonicalReduce`：推断归约操作。

### `src/ir/CanonicalIR.cpp`
- 主要功能：实现规范 IR 的类型和操作推断规则。
- 主要函数：
  - `CanonicalType::Bool`：构造布尔规范类型。
  - `CanonicalType::Bits`：构造位向量规范类型。
  - `CanonicalType::SignedView`：构造 signed view 规范类型。
  - `CanonicalType::hasSignedSemantics`：判断类型是否使用有符号语义。
  - `CanonicalType::key`：生成类型键。
  - `CanonicalType::str`：输出类型文本。
  - `canonicalBinary`：推断二元操作的 opcode 和结果类型。
  - `canonicalUnary`：推断一元操作的 opcode 和结果类型。

### `src/eval/PredicateEvaluator.h`
- 主要功能：声明 predicate 表达式解释执行器和位向量值。
- 主要函数：
  - `EvalBits::hex`：将位向量输出为十六进制。
  - `setVar`：设置变量值。
  - `setLookupTable`：设置 lookup 表值。
  - `eval`：求值表达式。
  - `fromUInt64`：从 64 位整数构造位向量。
  - `fromLiteral`：从字面量构造位向量。

### `src/eval/PredicateEvaluator.cpp`
- 主要功能：实现 predicate 表达式的位精确解释执行。
- 主要函数：
  - `EvalBits::hex`：格式化位向量。
  - `PredicateEvaluator::eval`：递归求值表达式。
  - `PredicateEvaluator::fromUInt64`：构造定宽位向量。
  - `PredicateEvaluator::fromLiteral`：解析整数字面量。
  - `mask`：按位宽裁剪值。
  - `add`/`sub`/`mul`：执行定宽算术。
  - `bitwise`：执行按位逻辑。
  - `shl`/`lshr`/`ashr`：执行移位。
  - `slice`：执行静态切片。
  - `writeSlice`/`writeBit`：执行切片或位写。
  - `concat`：执行拼接。
  - `compareUnsigned`/`compareSigned`：执行无符号/有符号比较。

### `src/semantics/IntSemantics.h`
- 主要功能：声明硬件整数操作的结果类型推断。
- 主要函数：
  - `binaryResultType`：推断二元整数操作结果类型。
  - `unaryResultType`：推断一元整数操作结果类型。

### `src/semantics/IntSemantics.cpp`
- 主要功能：实现 Int/fixint 操作类型语义。
- 主要函数：
  - `binaryResultType`：根据操作符、位宽和 signed view 推断二元结果。
  - `unaryResultType`：根据操作符和操作数推断一元结果。

### `src/semantics/AliasGraph.h`
- 主要功能：定义引用、指针、字段和数组元素别名关系图。
- 主要函数：
  - `bind`：绑定别名到目标。
  - `bindReference`：绑定引用别名。
  - `bindPointer`：绑定指针别名。
  - `bindField`：绑定字段别名。
  - `bindFieldPath`：绑定多级字段路径别名。
  - `bindArrayElement`：绑定数组元素别名。
  - `resolve`：解析别名目标。
  - `resolveField`：解析字段别名目标。
  - `resolvePath`：解析字段路径目标。
  - `has`：判断别名是否存在。
  - `recordUnsupported`：记录不支持的别名用法。

### `src/semantics/AliasGraph.cpp`
- 主要功能：实现别名关系图的绑定、补全和解析。
- 主要函数：
  - `AliasGraph::bind`：保存别名绑定。
  - `AliasGraph::bindReference`：保存引用绑定。
  - `AliasGraph::bindPointer`：保存指针绑定。
  - `AliasGraph::bindField`：保存字段绑定。
  - `AliasGraph::bindFieldPath`：保存字段路径绑定。
  - `AliasGraph::bindArrayElement`：保存数组元素绑定。
  - `AliasGraph::resolve`：解析直接别名。
  - `AliasGraph::resolveField`：解析单字段别名。
  - `AliasGraph::resolvePath`：解析多级字段别名。
  - `AliasGraph::recordUnsupported`：记录诊断信息。

## Emitter

### `src/emitter/ListJsonEmitter.h`
- 主要功能：声明 predicate 程序到 listjson 的输出接口。
- 主要函数：
  - `emitListJson`：输出 listjson 文本。

### `src/emitter/ListJsonEmitter.cpp`
- 主要功能：将 predicate 程序构造成单驱动信号列表并输出 JSON。
- 主要函数：
  - `emitListJson`：listjson emitter 入口。
  - `Builder`：内部构建器，负责信号、端口、操作和临时节点生成。
  - `emitType`：输出类型 JSON。
  - `emitOperand`：输出操作数 JSON。
  - `emitDebugLoc`：输出源位置 JSON。
  - `emitDebugInfo`：输出调试信息 JSON。
  - `emitOperation`：输出操作 JSON。

## BEIR 后端与优化

### `src/backend/beir.hpp`
- 主要功能：定义单驱动信号列表后端 IR、节点 ID、操作、类型、调试信息、facts 和可变优化图。
- 主要函数：
  - `DebugInfo::hasSourceLoc`：判断调试信息是否包含源位置。
  - `addDebugLoc`/`addDebugLocs`：向 `DebugInfo` 追加去重且有上限的源码位置。
  - `addOperandDebugLocs`：从操作数对应 signal、driver 和版本化基名继承源码位置。
  - `Operand::Constant::isZero`：判断常量是否为 0。
  - `Operand::Constant::isOne`：判断常量是否为 1。
  - `Operand::Constant::isAllOnes`：判断常量是否全 1。
  - `Operand::Constant::isBoolTrue`：判断常量是否为布尔真。
  - `Operand::Constant::isBoolFalse`：判断常量是否为布尔假。
  - `Operand::Constant::fitsU64`：判断常量是否可放入 `uint64_t`。
  - `Operand::Constant::toU64`：将小常量转为 `uint64_t`。
  - `Program::findSignal`：按节点 ID 查找信号。
  - `Program::signal`：按节点 ID 获取信号并要求存在。
  - `sameType`：比较 BEIR 类型是否相同。
  - `isCommutativeOp`：判断操作是否可交换。
  - `hashCombine`：组合哈希值。
  - `MutableProgram::finish`：结束可变优化并返回 `Program`。
  - `MutableProgram::ensureValueFacts`：确保 value facts 已刷新。
  - `MutableProgram::markValueFactsDirty`：标记 value facts 需要刷新。
  - `MutableProgram::replaceAliases`：批量替换别名操作数。
  - `MutableProgram::compact`：删除非 live 节点并重映射 ID。
  - `buildProgram`：从 `PredicateProgram` 构建 BEIR。
  - `emitText`：输出 BEIR 文本。

### `src/backend/beir.cpp`
- 主要功能：实现 BEIR 构建、文本输出、签名哈希、可变图维护和 value facts 分析。
- 主要函数：
  - `addDebugLoc`/`addOperandDebugLocs`：集中维护优化流程中的 DebugLoc 继承、去重和版本化信号回溯。
  - `buildProgram`：BEIR 构建入口。
  - `emitText`：BEIR 文本输出入口。
  - `Builder`：内部构建器，负责从 predicate 表达式创建信号和操作。
  - `parseConstant`：将字面量解析为规范常量。
  - `operationKindForExpr`：将 AST 表达式映射到 BEIR 操作种类。
  - `parseBinaryOpCode`/`parseUnaryOpCode`：将前端操作符映射到 BEIR opcode。
  - `sourceDebug`：生成源代码调试信息。
  - `generatedDebug`：生成中间节点调试信息。
  - `factsInferOperation`：推断单个操作的常量和确定位 facts。
  - `factsTopologicalOrder`：为 facts 分析生成拓扑序。
  - `MutableProgram::ensureValueFacts`：按需刷新 value facts。
  - `MutableProgram::analyzeValueFacts`：按拓扑序分析所有节点 facts。
  - `MutableProgram::operationSignature`：生成 CSE 用操作签名。
  - `MutableProgram::replaceAliases`：应用别名替换。
  - `MutableProgram::compact`：压缩 live 节点集合。

### `src/backend/beopt.hpp`
- 主要功能：声明 BEIR 优化选项和优化入口。
- 主要函数：
  - `parseOptions`：解析优化参数。
  - `optimizeProgram`：按选项执行 BEIR 优化 pipeline。

### `src/backend/beopt.cpp`
- 主要功能：实现 BEIR 优化选项解析和 pass 调度。
- 主要函数：
  - `parseOptions`：将字符串选项转换为 `Options`。
  - `optimizeProgram`：依次运行赋值链、bit facts、代数化简、位宽化简、CSE 和 DCE。

### `src/backend/beopt_assign_chains.hpp`
- 主要功能：优化连续 assign 和同 guard mux 链。
- 主要函数：
  - `foldAssignChains`：折叠可替换的赋值链。
  - `foldSameGuardMux`：折叠相同 guard 嵌套 mux。
  - `symbolDriver`：查找符号操作数的驱动操作。
  - `sameOperand`：比较两个操作数是否等价。

### `src/backend/beopt_bitvalue.hpp`
- 主要功能：基于 value facts 折叠常量和位值确定的操作。
- 主要函数：
  - `propagateBitValues`：执行位值传播优化。
  - `rewriteOperation`：根据输出 facts 重写单个操作。
  - `makeLiteralFromFacts`：从完全确定的 facts 构造字面量。
  - `sameLowBits`：判断低位是否与原值完全一致。

### `src/backend/beopt_algebraic.hpp`
- 主要功能：消除 `+0`、`*1`、与全零/全一位运算等无用算术和逻辑驱动节点。
- 主要函数：
  - `simplifyAlgebraicIdentities`：执行代数恒等式优化。
  - `rewriteBinary`：重写可化简的二元操作。
  - `constantOf`：解析操作数对应的常量。
  - `isZero`/`isOne`/`isAllOnes`：判断常量形态。
  - `setAssign`：将操作改写为 assign。

### `src/backend/beopt_width.hpp`
- 主要功能：执行综合位宽优化、无效 cast/ext/trunc/slice 删除和操作数宽度归一。
- 主要函数：
  - `simplifyWidthOperations`：位宽优化入口。
  - `simplifyWidthOperation`：应用局部位宽化简规则。
  - `rewriteIdentity`：删除等宽无意义转换。
  - `rewriteSelectAfterWiden`：优化先扩展后截取。
  - `rewriteWidenAfterSelect`：优化先截取后扩展。
  - `topologicalOrder`：为位宽分析生成拓扑序。
  - `analyzeAssignedWidths`：向前推断节点实际赋值位宽。
  - `analyzeDemandWidths`：反向推断节点实际被使用位宽。
  - `applyComprehensiveWidths`：按赋值宽度和需求宽度改写图。
  - `normalizeOperationOperands`：为操作插入必要的扩展或截断临时节点。

### `src/backend/beopt_cse.hpp`
- 主要功能：执行公共表达式合并。
- 主要函数：
  - `mergeCommonExpressions`：用操作签名合并等价内部节点。

### `src/backend/beopt_dce.hpp`
- 主要功能：删除不可达或未被观察的死节点。
- 主要函数：
  - `eliminateDeadNodes`：从端口输出和可观察节点反向标记 live 节点并压缩图。

### `src/backend/rtlgen.hpp`
- 主要功能：声明 BEIR/predicate 到 SystemVerilog 的生成入口。
- 主要函数：
  - `emitSystemVerilog`：输出 SystemVerilog 模块。

### `src/backend/rtlgen.cpp`
- 主要功能：将 BEIR 单驱动信号图生成 SystemVerilog 模块。
- 主要函数：
  - `emitSystemVerilog`：RTL 生成入口。
  - `Emitter`：内部发射器，负责端口、wire、assign 和 debug 注释输出。
  - `logicType`：生成 SystemVerilog packed/unpacked 类型文本。
  - `constExpr`：生成 SystemVerilog 常量表达式。
  - `svBinaryOp`：映射 BEIR 二元 opcode 到 SystemVerilog 操作符。
  - `emitLocList`：输出输入 C++ 源位置列表。
  - `debugComment`：统一生成 RTL 注释，格式为 `loc: <输入 C++ 行位置>; message: <来源或生成说明>`。
  - `signalDebug`：为无直接 driver 的版本化信号回退到基名信号源码位置。
  - `writeSliceExpr`：静态切片写使用 Verilog 大括号拼接 `{high, value, low}` 生成 RTL。
  - `emitDynamicWriteAssignment`：动态切片/位写生成一行 `always_comb begin base-copy; target[index +: N] = value; end`。

## Debug

### `src/debug/DebugLoc.h`
- 主要功能：定义前后端共享的源码位置调试信息。
- 主要函数：无独立主要函数。
