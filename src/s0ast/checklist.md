# S0AST V2 Planning Checklist

本文档只做 S0AST 的需求拆分和确认，不实现代码。S0AST 是 V2 pipeline 的新入口，允许新增全新的 AST 数据结构和流程，并直接接入 V2。

## 早期 AST builder 承担过的功能

1. 调用 libclang 解析 C++ 源文件或内存源文本，自动补 `fixint.hpp` wrapper，并定位 top function。
2. 识别 `TypeInfo`：`bool`、C++ builtin 整数、`Int<N>`、`UInt<N>`、`IntSignedView<N>`、`std::array<T,N>`、record/struct、enum underlying type。
3. 构造 `FunctionAST`：top body、helper function definitions、local lambdas、nested lambdas、struct field metadata、constructor metadata、global const/static decls。
4. 构造表达式/语句：literal、var、binary/unary、call、cast、ternary、array/field access、decl/assign/if/loop/switch/return/block/break/continue。
5. 识别部分 fixint/vullib API：`at`、`pick/range_at/bit_at`、`Cat/Repeat/Reduce*`、`zext/trunc/to/sint`，以及静态/动态 slice/write 类操作。
6. 含有旧路径兼容逻辑：RegProxy/ReqHelper/Queue/BRAM 风格识别、source-text fallback、operator receiver 恢复、proxy alias/constructor metadata 等。
7. 在 AST builder 内做了早期 subset 检查和递归检查，例如禁用 `new/delete/throw/try/floating point/I/O/function pointer/STL container`，检查 switch fallthrough、明显递归等。

## 原实现中应优化的问题

1. 单体过大：`ASTBuilderDriver.cpp` 同时做 libclang cursor 遍历、语义判断、API 识别、旧兼容恢复、部分 lowering、subset validate，职责边界不清。
2. AST 数据结构过宽：`Expr`/`Stmt` 使用大 union-like struct，很多字段只对某个 kind 有效，后续阶段容易误读无关字段。
3. use-level 语义与 type/storage 混杂：例如 `.sint()` 这种 operand view 容易被编码成 `TypeInfo`，但长期应作为 operand-use metadata 传播。
4. lambda/operator() 恢复脆弱：当前依赖 USR/location/signature/token fallback 组合，真实 C++ lambda 调用可能变成 `__unsupported_operator_call_receiver`。
5. helper/lambda/overload 的身份不够稳定：旧 AST 主要以名字和散列表组织，难以清晰表达 function entity id、lambda declaration id、callee resolution result。
6. 初始化语义不规范：decl init、constructor init、aggregate init、default construction 混在 Decl 节点字段中，S1 后续又需要重新拆为 Decl + Assign/Construct。
7. API lowering 过早/过散：原 AST 已经包含硬件 op kinds，同时 S1APINorm 又承担 API normalize；S0V2 应只保留 C++ surface AST 或明确的 raw API call，不做 S1 职责。
8. 旧 proxy 支持污染 V2：RegProxy/ReqHelper 等不再支持的机制仍存在于 AST 识别和 metadata 收集中，V2 应显式拒绝，而不是生成可继续传播的节点。
9. 错误诊断缺乏结构化上下文：部分错误只返回字符串；V2 应统一使用 `RTLZZException/ErrorContext`，携带 stage、source loc、cursor/semantic note。

## 子阶段拆分

S0AST 建议拆成以下子阶段，并分别确认语义：

1. `S0.0 ClangSessionAndRawCursor`：建立 libclang parse session，收集 source-owned declarations、diagnostics、cursor handles/source ranges，不构造语义 AST。
2. `S0.1 V2ASTDataModel`：定义收敛后的 V2 AST/type/entity/call/lambda 数据结构。
3. `S0.2 TypeAndEntityCollect`：收集并 canonicalize type/function/helper/lambda/struct/global const metadata。
4. `S0.3 ExprStmtBuild`：从 cursor 构造 V2 expression/statement tree，恢复 operator/lambda call，但不做 API lowering。
5. `S0.4 ResolveAndSurfaceValidate`：解析名字、overload、lambda/helper callee identity，并做 S0 级 surface subset 拒绝。
6. `S0.5 PipelineBridge`：定义 S0V2 到 S1APINorm/S2Validate 的接入方式、debug print、测试入口和失败边界。

## 待确认文件

请逐项确认以下文件中的语义问题：

- `checklist_0.md`: S0.0 Clang session/source ownership/raw cursor collection
- `checklist_1.md`: S0.1 V2 AST data model
- `checklist_2.md`: S0.2 type/entity/metadata collection
- `checklist_3.md`: S0.3 expression/statement construction
- `checklist_4.md`: S0.4 name/call resolution and surface validation
- `checklist_5.md`: S0.5 V2 pipeline bridge and tests
