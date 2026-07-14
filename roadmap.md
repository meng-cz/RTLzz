Proxy/引用字段清理方案：旧路径中的 RegProxy/ReqHelper/Queue/BRAM proxy 识别和字段引用绑定主要应从 normalize/AliasGraph 路径移除；如果保留现有 src/ast，则 AST 阶段需要收紧为“识别并拒绝 struct/array 内含 reference 或 pointer 字段”，同时保留顶层端口的 const-ref/input 与 mutable-ref/output 参数形式，删除或停用为 proxy constructor 收集字段到参数别名映射的特殊用途，让后续阶段不再接收 proxy carrier 或引用字段 struct。

  1. ParseAST
     用 libclang 解析 C++/fixint 子集，生成带类型、helper/lambda、struct 元数据的高层 FunctionAST，但不做 inline、flatten 或控制流 lowering。

  2. ASTValidateAndResolve
     校验支持的 C++ 子集，解析名字/作用域/重载结果，确认 helper/lambda/struct 元数据完整且无递归或非法参数形式。

  3. ScopeRename
     给 top/helper/lambda 的局部变量、临时变量、lambda 捕获和生成变量做全局唯一命名，消除 shadowing 与 block scope 名字冲突。

  4. ExpressionStatementize
     将复杂表达式中的 call、带副作用表达式、构造调用和求值顺序敏感表达式提升为显式临时变量和 statement-level 操作。

  5. StructuredControlNormalize
     将 return、break、continue、switch、短路逻辑等结构化控制语义规整成后续 CFG 能无歧义表达的形式。

  6. LoopLowerOrUnroll
     对静态可展开循环进行展开，或把循环转换为明确 CFG loop 结构；当前硬件子集可先要求所有循环在此阶段完全 unroll。

  7. BuildFunctionCFGs
     为 top/helper/lambda 分别构建 per-function CFG，基本块中只保留顺序语句和 statement-level call，控制流边显式表达分支/退出。

  8. LowerFunctionExits
     在 CFG 层把每个函数的多处 return 合并为明确的 function exit blocks，并把 return value 写入显式 return slot。

  9. InlineCallsCFG
     在 CFG 层通过 clone callee CFG、绑定参数、重命名局部、连接 return blocks 到 caller continuation 来内联 helper/lambda 调用。

  10. AliasAndParamLowering
     明确 value / const-ref / mutable-ref / pointer 参数语义，把引用别名、输出端口、proxy carrier 关系降低为统一的 lvalue/value 绑定模型。

  11. AggregateFlatten
     将 struct、array、aggregate init/copy、field access、array access、动态索引读写全部降低为确定的 scalar leaf 变量、mux 或 guarded write。

  12. LValueLowering
     将剩余复杂左值操作，如 bit/slice 写、动态位写、proxy lvalue、array element write，降低为显式读改写或 leaf assignment。

  13. OperationNormalize
     规范化 Int/UInt/builtin 整数语义、宽度扩展/截断、cast、slice、bit、concat、repeat、reduce、比较和算术操作。

  14. EffectAndDefaultPolicy
     处理输出端口默认值、写使能默认 false、ReqHelper/RegProxy/BRAM/Queue 等特殊硬件 side effect 的默认和配对策略。

  15. BuildSSA
     对 scalar CFG 做 SSA 转换，插入/表示 phi 或等价 merge，形成每个变量版本和控制流合流点的明确数据依赖。

  16. PredicateLowering
     将 SSA CFG 的控制流 lowering 为 predicate/guarded assignments，生成 predicate-friendly 的中间程序。

  17. PredicateVerifyAndSimplify
     校验无未初始化读、无多驱动冲突、输出覆盖完整、宽度一致，并做基础 guard/表达式简化。

  18. BuildBackendIR
     将 predicate program 转换为 backend IR/BEIR，形成后端所需的 signal、operation、assignment、register/memory/proxy effect 表示。

  19. BackendIROptimize
     在 backend IR 上做常量折叠、代数化简、CSE、DCE、宽度规整、predicate sinking 等后端优化。

  20. EmitBackendIR
     输出最终 backend IR 文本/listjson，或把 backend IR 交给 RTL emitter 继续生成 SystemVerilog。
