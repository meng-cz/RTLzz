Proxy/引用字段清理方案：旧路径中的 RegProxy/ReqHelper/Queue/BRAM proxy 识别和字段引用绑定主要应从 normalize/AliasGraph 路径移除；如果保留现有 src/ast，则 AST 阶段需要收紧为“识别并拒绝 struct/array 内含 reference 或 pointer 字段”，同时保留顶层端口的 const-ref/input 与 mutable-ref/output 参数形式，删除或停用为 proxy constructor 收集字段到参数别名映射的特殊用途，让后续阶段不再接收 proxy carrier 或引用字段 struct。

  0. ParseAST
     用 libclang 解析 C++/fixint 子集，生成带类型、helper/lambda、struct 元数据的高层 FunctionAST，但不做 inline、flatten 或控制流 lowering。

  1. APINormalize
     将 C++/fixint API call 规范化为 Hardware OP。

  2. ASTValidateAndResolve
     校验支持的 C++ 子集，解析名字/作用域/重载结果，确认 helper/lambda/struct 元数据完整且无递归或非法参数形式。

  3. ExpressionStatementize
     将复杂表达式中的 call、带副作用表达式、构造调用和求值顺序敏感表达式提升为显式临时变量和 statement-level 操作。为临时变量分配作用域无关的Symbol ID。

  4. BuildFunctionCFGs
     为 top/helper/lambda 分别构建 per-function CFG，基本块中只保留顺序语句和 statement-level call，控制流边显式表达分支/退出。

  5. LoopLowerOrUnroll
     对静态可展开循环进行展开，当前硬件子集可先要求所有循环在此阶段完全 unroll。

  6. InlineCallsCFG
     在 CFG 层通过 clone callee CFG、绑定参数、重命名局部、连接 return blocks 到 caller continuation 来内联 helper/lambda 调用。

  7. AggregateFlatten
     将 struct、array、aggregate init/copy、field access、array access、动态索引读写全部降低为确定的 scalar leaf 变量、mux 或 guarded write。处理复杂左值，生成显式的 leaf assignment。

  8. OperationNormalize
     规范化 Int/UInt/builtin 整数语义、宽度扩展/截断、cast、slice、bit、concat、repeat、reduce、比较和算术操作。

  9. BuildSSA
     对 scalar CFG 做 SSA 转换，插入/表示 phi 或等价 merge，形成每个变量版本和控制流合流点的明确数据依赖。此处需要拆分 lookupwrite到逐元素mux。

  10. PredicateLowering
     将 SSA CFG 的控制流 lowering 为 predicate/guarded assignments，生成 predicate-friendly 的中间程序。

  11. BuildBackendIR
     将 predicate program 转换为 backend IR/BEIR，形成后端所需的 signal、operation、assignment、register/memory/proxy effect 表示。
