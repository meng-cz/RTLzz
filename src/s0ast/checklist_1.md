# S0.1 V2ASTDataModel Checklist

目标：定义比旧 `FunctionAST` 更收敛的 V2 AST 数据结构，让 S1/S2 以后不再依赖原 AST 的宽松 union-like 字段和旧 proxy 语义。

## 需要确认的语义点

1. V2AST 是否继续复用 `pred::TypeInfo`？
   - 第一版定义 `S0Type`；避免继续把 storage type、view type、reference/passing、array dims 混在一个结构里。

2. 类型是否区分 storage type 与 use view？
   - 必须区分。`Int<N>` 是 storage scalar；`.sint()` 是 expression operand view metadata，不是变量或 symbol 类型。

3. V2 expression 是否允许原 AST 那种宽 struct？
   - 不允许。使用 `std::variant` 或分 kind 专用 struct，保证每个节点只含有效字段。

4. Literal 应如何表示？
   - S0 保留 source text、clang inferred type、source loc；不解析成 limb vector，交给 S8。

5. Declaration 节点是否允许 init 字段？
   - S0 可以保留 C++ surface init kind，后续 S1 规范化为 Decl + Assign/Construct。

6. Function identity 如何表示？
   - 每个 top/helper/lambda 都有稳定 `FunctionId`，另保存函数签名等；call 节点最终引用 `FunctionId` 或 unresolved callee candidate。

7. Lambda identity 如何表示？
   - lambda 作为 local function entity，有 `FunctionId`，并记录 保存函数签名，declaring function、source variable name、capture list、operator() signature。

8. Captures 是否作为 hidden params 表示？
   - S0 数据结构显式记录 captures；是否 lowering 为 hidden params 留到 S0.4 前确认，但不要把 capture object 当作普通 `auto` local。

9. Call 节点应保存哪些信息？
   - 保存 callee spelling、call syntax kind（free/member/operator/lambda call/constructor）、receiver expr、args、template args、resolved callee optional。

10. API call 与 HardwareOp 是否在 S0 中区分？
    - S0 不生成 HardwareOp；只把 fixint API 表示为普通 call/member call，并保留 template args/receiver。S1 负责 lowering。

11. Array/field access 是否保留复杂 lvalue tree？
    - 保留 C++ surface lvalue tree，但每个 access 节点要明确 read/write context、base type、index expression。

12. `struct`/aggregate metadata 在 S0 输出中如何表达？
    - 独立 `StructDecl` table，字段按 source order，构造函数 metadata 独立；同时标记是否含 pointer/reference/unsupported field。

13. 是否保留 switch？
    - 保留 surface `SwitchStmt`；S4 后续再 lowering CFG。

14. 是否保留 source-order children？
    - 必须保留，尤其用于 evaluation order、statementize、diagnostics。

15. V2AST 是否应可 debug print round-trip？
    - 需要稳定 debug print，但不要求可重新解析；用于 fixture 和阶段测试。

