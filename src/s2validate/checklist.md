# s2validate 语义澄清清单

本阶段目标暂定为：在 `ParseAST` 之后，对当前 `FunctionAST` 做支持子集校验、名字/作用域/调用目标解析一致性检查、helper/lambda/struct 元数据完整性检查，并在进入后续 `ScopeRename`、`ExpressionStatementize`、CFG 构建和 inline 前尽早拒绝不支持语义。

下面问题需要先明确，确认后再开始编码。

## 1. 阶段边界

1. `s2validate` 是否只做校验并返回错误，不修改 `FunctionAST`？
   - 只读校验，不改 AST；任何 AST 规整放到后续 `ScopeRename` / `ExpressionStatementize`。

2. `s2validate` 是否允许补全派生信息，例如构建 symbol table、call graph、struct dependency graph，并放入新的 result 结构？
   - 不生成，仅检查。

3. `s2validate` 是否应替代 `ASTBuilderDriver.cpp` 里现有的 `checkSubset`、helper recursion check、top param check？
   - 可以参考现有实现，但后续会完全替代现有实现成为新的流程。

4. 该阶段错误是否必须包含 source debug location？
   - 按照debug中RTLZZ异常体系抛出。

## 2. 顶层函数与参数

1. 顶层函数是否必须返回 `void`？
   - 是；所有硬件输出通过 mutable reference/output 参数表达。

2. 顶层参数允许哪些 passing kind？
   - 允许：
     - value input
     - const reference input
     - mutable reference output
   - 禁止：
     - pointer
     - rvalue reference
     - mutable reference input/inout 模糊语义

3. 顶层 `std::array<T, N>&` 输出是否继续允许？
   - 允许，但数组元素类型必须是bool, 内置整数 或 Int<Width>，且维度静态已知。

4. 顶层 struct 参数是否允许？
   - 禁止，顶层端口必须是bool或Int<Width>或它们的array。

5. 顶层参数是否允许 builtin integer，例如 `uint8_t& out`？
   - 允许，由类型系统统一映射为固定宽度硬件 scalar。

6. 顶层参数是否允许 `bool&` 输出？
   - 允许。

7. 顶层参数是否允许 `const T*` 或数组退化指针？
   - 不允许；所有数组必须是 `std::array`, `array` 的静态维度形式。

## 3. helper 函数

1. helper 是否允许返回非 `void` 值？
   - 允许，后续 CFG inline 通过 return slot 处理。

2. helper 是否允许返回 struct/array aggregate？
   - 允许，但 aggregate 类型不得含引用/指针，后续 flatten。

3. helper 参数允许哪些 passing kind？
   - 允许：
     - value
     - const reference
     - mutable reference
   - 禁止：
     - pointer
     - rvalue reference

4. helper 的 mutable reference 参数是否允许作为 inout？
   - 允许，这个参数不具备实际硬件语义，后续会被值链接或直接引用替换。

5. helper 是否允许重载？
   - 允许同名 helper 重载。

6. helper 是否允许递归或互递归？
   - 禁止直接递归与间接递归。

7. helper 是否允许调用未定义函数？
   - 未知 call 报错，目前输入中已经不存在intrinsic API调用。

8. helper 是否允许声明 static local？
   - 不允许，除非明确作为 lookup table。

## 4. lambda

1. lambda 是否作为 helper 等价处理？
   - 是，lambda 进入同一 call graph，禁止递归。

2. lambda capture 允许哪些形式？
   - 允许按值， const-ref， mutable reference capture。

3. lambda 是否允许返回 aggregate？
   - 允许，与 helper 一致。

4. lambda 是否允许嵌套 lambda？
   - 允许解析为独立 callee，但必须签名唯一、调用目标可解析。

5. lambda operator() 的隐藏调用是否应在 s2validate 阶段确认可解析？
   - 是；无法确定具体 lambda callee 时直接报错。

## 5. struct / array 类型规则

1. 是否全面禁止 struct 字段为 reference 或 pointer？
   - 是。

2. 是否全面禁止 array 元素类型含 reference 或 pointer？
   - 是，包括 nested array/struct。

3. 是否允许 struct 字段为 array 或 nested struct？
   - 允许，只要所有 leaf 类型合法且静态可展开（不允许[]数组，仅允许(std::)array）。

4. 是否允许 struct constructor？
   - 允许普通 value constructor；禁止任何 reference/pointer 字段，因此 constructor alias metadata 不再用于语义。

5. 是否允许空 struct？
   - 禁止，避免 zero-width aggregate。

6. 是否允许动态大小 array、`std::vector`、C pointer array？
   - 禁止。

7. 是否允许 top/helper 局部变量为 aggregate 且无初始化？
   - 需要明确未初始化检查归属。s2validate 只允许语法形态，未初始化读由后续 dataflow/verify 阶段处理。

## 6. Proxy / 特殊结构体机制

1. 是否彻底移除 `__RegProxy` / `__ReqHelper` / `QueueProxy` / `__BRAMProxy` 的 struct-name 特判？
   - 是，s2validate 遇到这些旧 proxy carrier 类型时直接报错。

2. 是否仍保留某些特殊硬件 side-effect API，例如 reg write、queue enqueue、bram write？
   - 禁止，这些API会在进入本项目前被替换。

3. `AliasGraph` 是否还需要支持局部 const reference alias？
   - 需要支持alias。

## 7. 控制流子集

1. `return` 是否允许出现在任意 helper/lambda 分支中？
   - 允许，由后续 CFG/LowerFunctionExits 处理；s2validate 只检查 top 函数 `return` 规则。

2. 顶层函数中是否允许 `return;` early exit？
   - 允许。

3. 是否允许 `break` / `continue`？
   - 只允许出现在 loop/switch 合法作用域内。

4. 是否允许 `switch` fallthrough？
   - 允许隐式 fallthrough。

5. 是否允许 `goto`？
   - 禁止。

6. 是否允许异常、`try/catch`、`throw`？
   - 禁止。

7. 是否允许短路逻辑 `&&` / `||` 中包含 call 或副作用？
   - 允许，后续CFG应当处理短路。

## 8. 表达式与副作用

1. 是否允许赋值表达式嵌套在表达式内部？
   - 允许，以C++语法为准。

2. 是否允许 `++` / `--`？
   - 允许。

3. 是否允许逗号表达式？
   - 禁止。

4. 是否允许条件表达式 `?:`？
   - 允许。

5. 是否允许 constructor call 出现在普通表达式中？
   - 允许。

6. 是否允许隐式类型转换？
   - 允许 AST 中保留，但后续 `OperationNormalize` 负责宽度语义；s2validate 只拒绝无法定宽类型。

## 9. 名字/作用域/调用解析

1. s2validate 是否需要确认所有 `VarRef` 都能解析到可见 symbol？
   - 是。

2. 是否允许 shadowing？
   - 允许，但不允许作用域内重定义。

3. 是否允许 helper/local 变量与 top 参数重名？
   - 允许，由 `ScopeRename` 消除；但同一 lexical scope 内重名禁止。

4. 是否允许函数名与变量名重名？
   - 允许。

5. 是否需要验证 `Call` 节点的 argument count 与 callee params 一致？
   - 是，普通 helper/lambda 必须严格一致。

## 10. 输出与接口

1. s2validate 的 API 返回什么？
   - 返回遇到的第一个 ValidateError，和一个vector<ValidateWarning>。

2. 是否应保留全部 warning，还是遇到第一个 error 就返回？
   - 遇到第一个 error 返回。

3. 是否需要 debug print？
   - 根据参数，打印 helper/lambda 列表、call graph、struct 合法性、top params。

4. 是否需要单独测试 fixture？
   - 需要，至少覆盖合法 top/helper/lambda、递归错误、struct 引用字段错误、未知 call、非法 top param。放置在目录testv2/fixtures/s2validate 中

