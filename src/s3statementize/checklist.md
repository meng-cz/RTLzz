# s3statementize 语义澄清清单

本阶段目标暂定为：在 `s2validate` 之后，将复杂表达式中的 call、带副作用表达式、构造调用、短路逻辑和求值顺序敏感表达式提升为显式临时变量与 statement-level 操作；输出结果中不应再存在复杂表达式树结构，后续 CFG/inline/flatten/normalize 只处理简单表达式和显式 statement。

下面问题需要先明确，确认后再开始编码。

## 1. 阶段边界

1. `s3statementize` 输入是否是当前 `FunctionAST`？
   - 是，输入为 `FunctionAST`，输出为新的 statementized program/result。

2. 输出是否仍使用 `FunctionAST`/`Stmt`/`Expr`，还是定义新的 Statementized IR？
   - 参考源AST定义新的更收敛 IR，避免用 `ExprKind::Call` 混在任意表达式位置。

3. 是否处理 top/helper/lambda 全部函数体？
   - 是，后续 CFG/inline 都需要 statementized callee。

4. 本阶段是否修改原 AST？
   - 不修改，输出新结构。

5. 错误是否使用 `RTLZZException` / `ErrorContextGuard`？
   - 是，阶段名固定为 `s3statementize`。

## 2. “不再存在复杂表达式树”的精确定义

1. 输出表达式允许哪些 `ExprKind`？
   - 需要明确。
   - 建议简单表达式允许：
     - `Literal`
     - `VarRef`
     - `ArrayAccess`，但 base/index 必须简单
     - `FieldAccess`，但 base 必须简单
     - `Cast`，但 operand 必须简单
     - 简单 unary/binary/ternary 允许

2. 是否允许 `BinaryOp` / `UnaryOp` 继续嵌套？
   - “不应存在复杂表达式树”是严格要求，建议每个 op 都提升为 temp：
     ```cpp
     t0 = a + b;
     t1 = t0 * c;
     ```
   - 确认采用这种三地址码风格。

3. 是否允许 `Ternary` 保留为表达式？
   - 如果 `?:` 代表控制流（内部存在函数调用），应转为 branch；如果只作为 mux，可保留但 cond/arms 必须简单。

4. 是否允许 `Concat/Slice/BitSelect/Reduce/Repeat` 等硬件表达式继续嵌套？
   - 可作为 simple op statement 的 RHS，但其 operands 必须简单。

5. 输出中是否允许任何 `ExprKind::Call`？
   - 只允许出现在显式 `CallStmt`，不允许出现在普通 expression tree。

## 3. Statementized IR 形态

1. 是否需要新增 statement 类型而不是复用 `StmtKind`？
   - 新增，例如：
     ```cpp
     DeclStmt
     AssignStmt
     CallStmt
     EvalStmt
     IfStmt
     ForStmt/WhileStmt/SwitchStmt 或保留结构控制
     ReturnStmt
     ```

2. `CallStmt` 应包含哪些字段？
   - 建议：
     - optional result variable/lvalue
     - callee name
     - args: simple expr list
     - result type
     - original debug loc

3. 普通运算是否也需要显式 `OpStmt`？
   - 采用三地址码，建议：
     ```cpp
     temp = op(simple operands)
     ```

4. constructor call 是否是 `CallStmt`，还是 `ConstructStmt`？
   - 普通 value constructor 用 `ConstructStmt`，方便后续区分 helper call。

5. output 是否还保留 struct/array/field access？
   - 保留 aggregate lvalue/rvalue 语义，但所有 path 组件必须 simple；后面 flatten 后再拆。

## 4. 临时变量规则

1. 临时变量命名规则是什么？
   - `__tmp_<function>_<target var name>_<counter>`，避免与用户名冲突。

2. 临时变量声明插在哪里？
   - 就近插入：首次赋值前生成 `Decl temp`，然后 `Assign/Call`。

3. 临时变量类型如何确定？
   - 如果不能确定应设置为unknown/temp，等待后续norm阶段处理。

4. 临时变量是否需要出现在 function symbol table 中？
   - 输出 IR 显式包含 temp decl，后续阶段自然可见。

5. 每个复杂表达式是否必须 materialize 成 temp，即使只用一次？
   - 全部 materialize，debug 更清晰。

## 5. 求值顺序

1. 函数调用参数求值顺序按什么规则？
   - C++20 对普通函数实参求值顺序仍有未指定部分。
   - 项目内定义为从左到右。

2. binary operator 左右操作数求值顺序按什么规则？
   - 采用 AST child order。

3. assignment RHS 和 LHS 的求值顺序如何处理？
   - 先 statementize RHS，再 statementize LHS address/path。

4. array index 与 base 表达式求值顺序如何处理？
   - base 再 index。

5. `?:`、`&&`、`||` 是否需要保留短路求值语义？
   - 是；必要时提升为分支。

## 6. call 提升规则

1. helper/lambda call 在任意表达式位置都提升为 statement-level call？
   - 是。

2. call 返回值如何处理？
     - 表达式位置：创建 temp，`CallStmt{result=temp}`
     - assignment RHS：可直接 `CallStmt{result=target}`。
     - void call：仅 `CallStmt{result=nullopt}`

3. nested call 例如 `f(g(a), h(b))` 如何输出？
     ```cpp
     t0 = call g(a)
     t1 = call h(b)
     t2 = call f(t0, t1)
     ```

4. method/operator call 是否已在 AST 中是普通 `Call`？
   - 是。

5. unknown call 是否已经由 s2validate 禁止？
   - 是，s3 假设普通 call 都可解析或是 constructor。

## 7. constructor / aggregate init

1. declaration initializer 中的 constructor call 是否提升？
   - 例如 `Packet p = Packet{a, b};`
   - 明确输出是 `ConstructStmt p(...)`。

2. aggregate initializer args 中若有 call，是否逐个提升？
   - 是，按明确顺序生成 temps。

3. default constructor 是否需要显式 statement？
   - 生成 `ConstructStmt`，用于后续默认0初始化。

4. copy constructor / aggregate copy 是否作为普通 assignment？
   - 保留 aggregate assignment，后续 flatten。

## 8. 副作用表达式

1. 赋值表达式嵌套在表达式中如何 statementize？
   - s2validate 允许以 C++ 语法为准。
   - 明确：
     ```cpp
     x = (y = a) + b;
     ```
     变成：
     ```cpp
     y = a;
     x = y + b;
     ```

2. `++` / `--` 如何处理？
   - 明确 pre/post 语义。
     - `++x`: `x = x + 1; result = x`
     - `x++`: `result = x; x = x + 1`

3. compound assignment `+=`, `-=`, etc. 是否已在 AST 中变成 Assign/Binary？
   - 未变换，s3 负责降低。

4. comma expression 已在 s2validate 禁止，s3 是否仍需要防御性检查？
   - 是。

5. function call side effects 是否只通过 `CallStmt` 表达？
   - 是。

## 9. 短路逻辑与条件表达式

1. `&&` / `||` 是在 s3statementize 阶段变成 control-flow statement，还是保留给 CFG？
   - 必要时生成 structured `If`。

2. `a && f()` 如果 `f` 有副作用，如何表示？
   - 不能提前执行 `f`；生成 structured `If`。

3. `?:` 分支中有 call/side effect 如何表示？
   - 生成 temp + if/else assign。

4. 如果 `?:` 只是纯 mux，是否可以保留为 simple expr？
   - 可以。

## 10. 控制流语句中的表达式

1. `if` condition 如何 statementize？
   - condition 前插入必要 temps；最终 condition 是 simple expr。

2. `for` init/cond/step 如何 statementize？
   - 保留 `For`，s3 只处理表达式不改变控制结构。

3. `while` / `do while` condition 中的 call/side effect 如何处理？
   - 将 condition prelude 放进 loop condition block ，保持最终 condition 是 simple expr。while循环需要在循环体之前和循环体最后各插入一次 condition evaluation。

4. `return expr` 中 call/side effect 如何处理？
   - 先生成 temps/calls，再 `Return simple_expr`。

5. `switch expr` 如何 statementize？
   - 先生成 simple selector temp，再 switch。

## 11. 与 CFG build 的接口

1. s3statementize 输出是否仍保留结构化控制流，供后续 CFG build 转换？
   - 是；s3 只把表达式复杂度降到 statement-level。

2. 如果 s3 生成 `If` 来表达短路/ternary side effect，后续 CFG build 是否需要处理这些 synthetic if？
   - 是，并通过 debug loc/note 标记 synthetic。

3. 后续 CFG 是否应假设所有 condition 都是 simple expr？
   - 是。

4. 后续 inline 是否应只处理 `CallStmt`，不再扫描表达式中的 Call？
   - 是。

## 12. 与 ScopeRename 的关系

1. ScopeRename 是否在 s3statementize 之前？
   - 不是，s3 生成 temp 时必须避免与所有 visible user names 冲突。

2. s3 生成 temp 是否需要 lexical scope？
   - temp 声明在当前 statement 所在 block scope。

3. 如果展开 `?:` / short-circuit 生成 nested block，temp 的作用域如何保证覆盖使用点？
   - 通过调整 temp 声明位置所在的定义域与使用位置相同。

## 13. 后置验证

1. s3 输出后是否做 verifier？
   - 是。

2. verifier 需要检查哪些？
     - 普通 expression 中无 `Call`
     - expression depth 不超过允许范围
     - call args 都是 simple expr
     - condition/return/switch expr 都是 simple expr
     - no comma expr

3. 是否允许 complex lvalue？
   - 允许，例如 `arr[f(i)].x = y` 必须 statementize index call，但最终 `arr[t].x` 算 simple lvalue。

## 14. API 与测试

1. API 返回什么？
     ```cpp
     struct StatementizeResult {
         optional<StatementizedProgram> program;
         optional<Error> error;
         vector<Warning> warnings;
         string debug_text;
     };
     ```

2. 是否复用 `FunctionAST` 作为输出？
   - 否，定义更收敛的表示。

3. 是否需要 debug print？
   - 参数可选开启，打印每个函数生成的 temp、CallStmt、statementized body。

4. 测试目录放哪里？
   - `testv2/fixtures/s3statementize` 和 `testv2/s3statementize_test.cpp`。

5. 最小测试应覆盖哪些？
   - 建议至少覆盖：
     - `out = f(g(a), h(b))`
     - `return f(a) + 1`
     - `if (f(a)) ...`
     - `arr[f(i)] = g(x)`
     - constructor args 中含 call
     - `x = (y = a) + b`
     - `x++` / `++x`
     - `a && f(b)` 短路
     - `cond ? f(a) : g(b)`
     - top/helper/lambda 全部处理

## 用户补充：

Statementize结果数据结构中，不应再使用字符串表达如op等，需要用枚举明确支持的一元、二元、三元操作符。

表达式数据结构应当被重构，明确表达式-操作数的层级，操作数中不允许再嵌套表达式。

complex lvalue 需要独特的结构，允许field/array access，其应当是一个独立的vector，每个元素是一级access，每个access可以是field或array，array access的index必须是simple expr。
