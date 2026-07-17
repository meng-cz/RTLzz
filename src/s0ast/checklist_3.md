# S0.3 ExprStmtBuild Checklist

目标：从 libclang cursor 构造 V2 expression/statement tree，可靠恢复 C++ operator/member/lambda call 和初始化语义，但不做 S1 API lowering。

## 需要确认的语义点

1. 是否允许 source-text fallback 生成语义节点？
   - 不允许。token/source text 只用于 diagnostics 或 recovery hint；语义节点必须来自 cursor/type/reference 信息。

2. overloaded operator 如何表示？
   - 保留为 call syntax 或 normalized unary/binary op，必须记录实际 operator spelling 和每个 operand 的独立 type/view，不允许从父 cursor 扩散 signed_view。

3. lambda `operator()` 调用如何恢复？
   - call node 显式表示 `LambdaCall`，callee 指向 lambda variable/entity；不要生成 `__unsupported_operator_call_receiver` 占位进入后续阶段。

4. member call receiver 如何表示？
   - Call 节点包含 optional receiver expression；`x.at<7,0>()`、`x.pick<4>(i)`、`lambda.operator()(...)` 都统一为 receiver + callee + args + template args。

5. constructor/function-style cast 如何区分？
   - `Int<8>(x)` 作为 `ConstructExpr` 或 `CastConstructExpr`，不作为普通 helper call；S1 决定 ZExt/SExt/Trunc。

6. declaration initialization kinds 如何表达？
   - 区分 `NoInit`、`DefaultConstruct`、`CopyInit(expr)`、`DirectInit(args)`、`ListInit(args/designators)`。

7. assignment lhs/rhs 如何表达？
   - AssignStmt 的 lhs 必须是 lvalue expression；compound assignment 自身保留或拆为 op+assign需确认。

8. compound assignment 在 S0 是否拆解？
   - S0 保留 `CompoundAssign`，S3 statementize 再按 evaluation order lowering；但可在 S0 标注 equivalent binary op。

9. pre/post increment 在 S0 是否保留？
   - 保留 `UnaryUpdate` 节点，标注 pre/post 和 delta；S3 负责拆为 read/write。

10. logical `&&`/`||` 是否保持短路节点？
    - 保留 `LogicalAnd/LogicalOr` 短路语义，不急于变普通 binary op。

11. ternary `?:` 如何表示？
    - 保留 `ConditionalExpr`，记录 cond/then/else type；S3/S7 以后再处理 aggregate/lookup lowering。

12. switch fallthrough 是否在构建时拒绝？
    - 构建时可标记 fallthrough；不在 builder 内混入复杂校验。

13. for loop step 是否作为独立 field？
    - S0 surface AST 保留 C++ for init/cond/step/body；S4/S5 之后不应依赖 step 特殊性。

14. return value 如何表示？
    - ReturnStmt optional expr；void return 明确表示；early return 不在 S0 改写。

15. unsupported cursor 如何处理？
    - 立即生成 structured error，不输出 unsupported placeholder call/expression。

16. debug loc 粒度要求？
    - 每个 Expr/Stmt 都保存 begin/end file/line/col；operator token loc 可选但建议保存。

