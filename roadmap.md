输入端口 ABI：top 必须是无参数、返回 `void` 的函数。RTL 端口由同一源文件中的文件级全局变量声明，变量只能使用 `bool`、`Int<N>`、内置整数或这些标量的 `std::array` 形式，且不得带初始化器。每个全局变量必须由 `#pragma input_port name` 或 `#pragma output_port name` 唯一指定方向；未标注变量、无对应变量的 pragma、重复/冲突 pragma 均在 S0 报错。top、helper、函数模板特化和 lambda 均可直接访问全局端口；S0 将 top 访问收敛为内部 `ParamDecl`，并将 helper/lambda 的直接和传递端口依赖提升为隐式参数，后续阶段继续消费统一端口模型。局部硬件值没有隐式初值；若控制流或动态写需要读取旧值，必须由源码显式初始化，否则在 SSA 阶段报错。

Proxy/引用字段清理方案：旧路径中的 RegProxy/ReqHelper/Queue/BRAM proxy 识别和字段引用绑定主要应从 normalize/AliasGraph 路径移除；AST 阶段拒绝 struct/array 内含 reference 或 pointer 字段，并删除或停用为 proxy constructor 收集字段到参数别名映射的特殊用途，让后续阶段不再接收 proxy carrier 或引用字段 struct。

  0. ParseAST
     用 libclang 解析 C++/fixint 子集，收集文件级全局端口及方向 pragma，将其收敛为内部 top `ParamDecl`；识别 helper/lambda 对全局端口的直接和传递依赖，并提升为隐式参数、补全调用实参。生成带类型、helper/lambda、struct 元数据的高层 FunctionAST；不做 inline、flatten 或控制流 lowering。

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

  12. AutomaticPipeline（可选，BEOPT 后）
      CompileOptions 提供内部寄存器级数 N、时钟名、强类型运算延迟权重覆盖以及时序容差（默认 5%）。在优化后的活跃 DAG 上调度 N+1 个组合阶段，先搜索估计时序基准，再在最长级延迟不超过基准 × (1+容差) 的约束内通过确定性局部移动减少寄存器总位数。条件、索引、数据及数组依赖均按事务对齐，跨级延迟链共享，常量不插寄存器。单个运算不拆分。
      独立 pipeline::Program 保留组合 BEIR 和阶段/寄存器表；物化之后不运行组合 BEOPT。原生 RTL 发射器根据寄存器表生成上升沿 always_ff，输入输出仍为外部 Q/D 之间的 wire 边界，不生成边界寄存器。N=0 保持组合路径；N>0 对应端到端外部输出采样 N+1 周期。默认无复位，可选根据输入初值推导同步复位值；无握手，CIRCT 暂不支持流水模式。
      配置输入端口初值后，在调度／物化之后、RTL 发射之前，沿流水线 DAG 求值所有内部寄存器及输出，保留任意位宽与数组元素顺序。内部寄存器发射同步复位字面量；输出初值通过 PipelineReport::output_reset_values 返回外部边界框架。零内部级数也支持输出初值报告。默认不启用复位，不改变组合或既有无复位流水模式。

      若显式配置 input_valid_name/output_valid_name，frontend 保留 bool 端口并以常量占位定义后端独占的输出。调度时不将该占位输出当作数据路径；数据寄存器物化后建立 N 级专用 valid 链，数据寄存器绑定来源阶段的 valid 写使能，各阶段直接引用输入 valid 时接到对应链节点。valid 每周期推进，N=0 直接连接 valid 端口；复位优先于写使能。报告面积包含完整 valid 链。
