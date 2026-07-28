1.Clang18Session：负责建立基于 LLVM 18 Clang C++ API 的编译会话，显式配置 C++20、include path、vullib、用户 clang args、诊断消费者和 `CompilerInstance` 生命周期；该步骤只产出可访问的 `ASTContext`、`SourceManager`、主文件身份和 clang 诊断，不做 RTLzz 语义抽取。

2.SourceAndDebugLoc：负责统一源文件路径、宏展开位置、拼写位置和源码范围到 RTLzz `DebugLoc` 的转换规则，明确何时使用 expansion loc、spelling loc 和 presumed loc；所有后续 S0 构造的 `Expr`、`Stmt`、`ParamDecl`、struct metadata、临时解析记录都必须从这里取得 debug 信息。

3.PragmaAndPortDeclCollect：负责从预处理/AST 可见信息中收集 `#pragma input_port`、`#pragma output_port` 与文件级全局变量声明，校验 pragma 与变量一一对应、方向唯一、端口无初始化器、端口类型属于支持子集；该步骤只形成原始端口表，不修改函数签名。

4.TopFunctionSelect：负责按 CLI top 名称或通配规则在当前翻译单元中选择唯一 top 函数，校验源码级 top 是无参数 `void` 定义，并保留其 `FunctionDecl` 身份；若匹配为空、多义、声明非定义或签名非法，在此阶段给出带源码位置的 S0 错误。

5.SemanticIndexBuild：负责以 Clang declaration identity 为核心建立符号索引，包括 `NamedDecl`、`FunctionDecl`、`FunctionTemplateDecl`、`CXXMethodDecl`、lambda `operator()`、`VarDecl`、`FieldDecl`、record decl 与 template specialization 的稳定映射；后续解析必须优先使用 Clang 语义节点而非名称字符串。

6.TypeLowering：负责把 `clang::QualType` 规范化为 RTLzz `TypeInfo`，覆盖 `bool`、支持宽度的内置整数、`Int<N>`、`std::array<T,N>`、record/struct、cv/ref/pointer 的边界语义，并统一剥离 record 名称中的顶层 cv/ref 文本；该步骤只做类型表示转换和非法类型诊断，不构造表达式。

7.ConstEval：负责提供 S0 解析期整数和 bool 常量求值服务，优先使用 Clang AST 的 `Expr::EvaluateAsInt`、`EvaluateAsBooleanCondition`、`TemplateArgument` 与 `APValue`，支持 `Int<N>` 宽度、数组长度、模板实参、case label 和静态 unroll 所需边界；失败时返回精确诊断，不再依赖源码 token 拼接式求值。

8.RecordMetadataCollect：负责收集所有被 top、helper、lambda、端口和局部变量可达的 struct/class record metadata，包括字段顺序、字段类型、构造函数参数到字段写入关系、aggregate init 可用性和非法 reference/pointer 字段拒绝；metadata key 必须使用 canonical record decl/type，避免 `const T` 与 `T` 查找分裂。

9.FunctionReachabilityCollect：负责从 top 出发沿普通函数调用、函数模板特化调用、lambda 调用、generic lambda `operator()<...>` 调用和成员 helper 调用收集所有需要进入 S1-S6 的函数体；该步骤确定 helper/lambda 的唯一内部名称和实例化参数，但暂不转换函数体语句。

10.TemplateSpecializationResolve：负责把 Clang 已完成语义实例化的函数模板、成员模板和 generic lambda specialization 绑定到 RTLzz helper/lambda 实体，记录每个非类型模板参数的常量值、模板参数名称到值的环境，以及源调用点 debug loc；所有 `at<CONFIG - 1, 0>` 这类表达式模板参数应在这里通过 Clang 语义结果解析。

11.LambdaCaptureResolve：负责解析 lambda capture，包括显式捕获、默认引用/值捕获、this 捕获、嵌套 lambda 捕获和 template lambda 内部捕获；输出统一的显式参数需求表，使 lambda 函数体转换时只看到 RTLzz 支持的参数和局部变量，不依赖隐式闭包对象。

12.ExprBuild：负责把 Clang `Expr` 树转换为 V2 surface `Expr`，覆盖 literal、decl ref、member ref、array subscript、call、member call、operator call、unary/binary/ternary、cast、construct、init list、temporary object、field access、array access 和 fixint surface API；该步骤只保留源码语义形态，不做 statementize、flatten 或完整常量传播。

13.StmtBuild：负责把 Clang `Stmt` 树转换为 V2 surface `Stmt`，覆盖 declaration、assignment、compound assignment、expression statement、if、for、while、do while、switch/case/default、break、continue、return 和 block，保留 C++ 求值顺序所需的表达式结构；未初始化局部变量应保持“无初值”状态，不在 S0 隐式补默认值。

14.InitAndConstructBuild：负责统一处理变量初始化、aggregate init、designated init、`std::array` 初始化、struct 构造、临时 aggregate/struct 返回值直接读取和空 `{}` 初始化；该步骤必须区分语法上的“无初始化器声明”和显式初始化，前者不产生默认构造语义，后者走统一 construct/aggregate 表示。

15.CallAndAPIBinding：负责把普通 helper 调用、模板 helper 调用、lambda 调用、member helper 调用和 fixint/RTLzz 支持 API 调用绑定为稳定 callee 或 intrinsic surface form，记录调用点、接收者、模板参数和参数传递方式；未知函数、STL 容器运行期操作和不支持 API 在此阶段报错。

16.GlobalPortLift：负责把 top 中收集到的全局端口收敛为内部 `ParamDecl`，并沿 helper/lambda 调用图传播直接或传递端口依赖，将端口访问提升为隐式参数和补全调用实参；该步骤必须基于 declaration identity 处理同名遮蔽，保证 helper/lambda 直接读写端口或数组端口时与 top ABI 一致。

17.SurfaceValidate：负责对 S0 产出的 V2 surface AST 做前端边界校验，确认不存在未解析 callee、未知类型、非法引用/指针字段、非法端口、未实例化模板、无法转换的 Clang 节点和脱离支持子集的 C++ 结构；该步骤只检查 S0 输出自洽性，不重复 S2 的跨阶段语义验证。

18.S0ProgramBridge：负责把新 S0Clang18 的内部结果桥接到现有 `s0ast::S0Program` 和 `v2::FunctionAST` 消费接口，保持 S1-S11 的输入协议不变，并提供 debug print、错误诊断、异常上下文和与旧 libclang S0 的 A/B 对比入口；该步骤是迁移期间切换 feature flag 和回归测试的边界。

