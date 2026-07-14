# s4cfg 语义澄清清单

本阶段目标暂定为：在 `s3statementize` 之后，为 top/helper/lambda 分别构建 per-function CFG；基本块中只保留顺序语句和 statement-level call/construct/op/assign/decl 等非控制流 statement；所有结构化控制流由显式 CFG edge 和 terminator 表达。本阶段不做 inline、不做 loop unroll、不做 return slot 合并、不做谓词化。

下面问题需要先明确，确认后再开始编码。

## 1. 阶段边界

1. `s4cfg` 的输入是否是 `s3statementize::StatementizedProgram`？
   - 是。

2. 输出是否定义新的 CFG IR，而不是复用旧 `src/ir/CFG.{h,cpp}`？
   - 义 `src/s4cfg` 自己的 CFG IR，顺序语句内容复用s3statementize的结构。

3. 是否处理 top/helper/lambda 全部函数？
   - 是，输出 `CFGProgram{top, helpers, lambdas, struct metadata}`，为后续 CFG inline 做准备。

4. 本阶段是否修改输入 `StatementizedProgram`？
   - 不修改，输出新结构。

5. 错误是否继续使用 `RTLZZException` / `ErrorContextGuard`？
   - 是，阶段名固定为 `s4cfg`。

## 2. CFG IR 形态

1. block id 是否使用稳定递增整数？
   - 每个 function CFG 内部从 0 开始递增，debug print 使用 `bb0`, `bb1`。

2. 每个 function 是否必须有唯一 entry block 和 exit block？
   - 是；entry 可为空并 jump 到第一个真实 block，exit 无 successors。

3. basic block 内允许哪些 `S3StmtKind`？
   - 只允许顺序语句：
     - `Decl`
     - `Assign`
     - `Op`
     - `Call`
     - `Construct`
     - `Eval`
   - 不允许：
     - `If`
     - `For`
     - `While`
     - `DoWhile`
     - `Switch`
     - `Break`
     - `Continue`
     - `Return`

4. terminator 需要哪些种类？
   - 建议：
     - `Jump`
     - `Branch`
     - `Switch`
     - `Return`
     - `Unreachable`
     - `Exit`

5. CFG edge 是否需要带语义标签？
   - 需要，例如 `true`、`false`、`case <value>`、`default`、`fallthrough`、`break`、`continue`、`return`，便于 debug 和 verifier。

6. terminator 与 edge 信息是否重复保存？
   - terminator 保存语义目标，block 也保存 predecessor/successor edge list；verifier 检查二者一致。

## 3. 顺序语句与 block 切分

1. 连续顺序语句是否尽量合并到同一个 block？
   - 是，直到遇到控制流 statement 或当前 block 已有 terminator。

2. 空 block 是否允许？
   - 允许 entry/exit/merge/loop header 为空；普通空跳转 block 可先允许，后续可优化。

3. s3 生成的 `condition_prelude` 放在哪里？
   - 放入条件判断所在 header/condition block 的顺序语句区域，terminator 使用 prelude 后得到的 simple condition operand。

4. block 中是否允许 `CallStmt`？
   - 是，`CallStmt` 是 statement-level call，属于顺序语句，后续 inline 阶段只扫描 block 内 `CallStmt`。

5. `ConstructStmt` 是否也作为顺序语句保留？
   - 是，后续 aggregate/lvalue lowering 再处理。

## 4. If

1. `S3StmtKind::If` 如何转换？
   - 当前 block 先放 condition prelude 已在 s3 外置或 synthetic if 内无 prelude，然后用 `Branch(condition, then, else)`；then/else 末尾跳到 merge。

2. 没有 else body 时是否仍创建 else block？
   - 创建 explicit else/fallthrough block。

3. then/else 分支如果以 return/break/continue 结束，是否还连到 merge？
   - 不连；只有分支末尾可达时才 jump merge。

4. synthetic `If` 与 source `If` 是否需要在 CFG 中区分？
   - 不区分。

## 5. Loop

1. 本阶段是否保留 loop back-edge，而不是 unroll？
   - 建议：是；roadmap 后续 `LoopLowerOrUnroll` 单独处理。

2. `For` 的 CFG 形态是什么？
   - 建议：
     - init 顺序语句
     - cond/header block 执行 `condition_prelude`
     - cond true 到 body，false 到 loop exit
     - body 可达末尾跳 step
     - step 顺序语句后跳回 cond/header

3. `for` 缺省 condition 如何表达？
   - 无条件 jump 到 body。

4. `While` 的 condition prelude 如何保证每次迭代执行？
   - 已在s3处理，保证while的condition是simple的。请确认并优化s3的实现，其中将condition prelude statements挂载到while/dowhile上而不是作为正常的statement。

5. `DoWhile` 的 condition prelude 如何放置？
   - 已在s3处理，保证while的condition是simple的。请确认并优化s3的实现，其中将condition prelude statements挂载到while/dowhile上而不是作为正常的statement。

6. `continue` target 对不同 loop 的含义是什么？
     - `for`: continue 到 step block
     - `while`: continue 到 condition prelude block
     - `do while`: continue 到 condition prelude block

7. `break` target 是否统一指向当前 loop/switch exit block？
   - 是。

8. 后续若要求所有 loop unroll，本阶段是否仍允许非静态 loop 进入 CFG？
   - 允许进入，等后续再判断常量化。

## 6. Switch

1. `Switch` terminator 是否作为一等 terminator，还是降低为级联 branch？
   - 保留一等 `SwitchTerminator{selector, cases, default}`，让 CFG 结构更忠实；后续可 lowering。

2. `case` body 的 fallthrough 语义如何处理？
   - C++ switch 默认允许 fallthrough；s4 必须表达 fallthrough edge。

3. 多个 case label 指向同一 body 时如何表达？
   - 允许，应支持多个 case edge 到同一 block 或 case block fallthrough。

4. 没有 default 时 false/default edge 指向哪里？
   - 指向 switch exit block。

5. `break` inside switch 是否跳到 switch exit，且不影响外层 loop break？
   - 是，break target 栈需要区分 switch/loop；continue 仍只找外层 loop。

## 7. Return 与 Exit

1. 本阶段是否合并多 return 到 return slot？
   - 做；在一个单独的cpp中实现 return value slot 和统一 exit，调用顺序位于主cfg build之后。

2. `Return` terminator 是否保留 optional return operand？
   - 是；并加 `return` edge 到 function exit block。

3. `return` 后同一结构块内的后续 statement 如何处理？
   - 不可达；s4 可忽略 unreachable tail 并产生 warning。

4. void function 的 implicit fallthrough 是否等价于 return void？
   - 等价于。

5. non-void helper/lambda fallthrough 到 exit 是否允许？
   - 若没有，s4 verifier 应报错。

## 8. Break / Continue / Unreachable

1. `Break` 出现在非 loop/switch 中是否报错？
   - 报 `RTLZZException`。

2. `Continue` 出现在非 loop 中是否报错？
   - 报 `RTLZZException`。

3. `break/continue/return` 之后同一 statement list 中还有 statement 时如何处理？
   - 后续 statement unreachable，可忽略并 warning。

4. 是否需要显式 `Unreachable` terminator？
   - 仅用于 debug/verifier 内部表示，不主动生成，除非输入含不可达空分支。

## 9. Declaration 与 Scope

1. block scope 是否需要在 CFG 中保留？
   - s3 把 temp decl 放在当前 block scope；CFG 打散后，decl 的支配关系可能影响合法性。需要显式 scope marker。

2. 分支内部 decl 的作用域如何表达？
   - 先作为 block 内 `Decl` 保留，后续 SSA 负责验证 use 不逃逸；但需要确认是否已有作用域阶段。

3. temp decl 如果位于 `condition_prelude`，其可见范围是否只需覆盖 condition operand？
   - 是；condition block 内 decl 支配 terminator 即可。

4. 同名局部是否已由 s2 或 s3 temp 规则避免？
   - s2 只禁止同一 lexical scope 重定义；尚未 scope rename。s4 必须携带有效的 scope id 列表。

## 10. Function Program 结构

1. helper/lambda 的存储方式是否沿用 s3？
   - 组织方式参考 s3，但数据结构应当不同。

2. function CFG 是否保留 params/return_type？
   - 保留，供后续 inline、return slot、param lowering 使用。

3. struct metadata 是否原样透传？
   - 是，`struct_fields` / `struct_constructors` 原样复制。

4. 是否需要 function name 到 CFG 的索引？
   - 提供 helper/lambda lookup map 或在 result 中构建 name index，供后续 inline 使用。

## 11. Verifier

1. s4 输出后是否做 verifier？
   - 是。

2. verifier 需要检查哪些？
   - 建议至少检查：
     - entry/exit 存在
     - block id 与 vector index 一致
     - block 内无控制流 `S3StmtKind`
     - terminator target 全部有效
     - predecessors/successors 与 terminator edge 一致
     - exit block 无 successors
     - 非 exit block 必须有 terminator
     - `Branch` condition 是 simple operand
     - `Switch` selector/case value 是 simple operand
     - `Return` operand 是 simple operand
     - `Break`/`Continue` 不残留在 block 内

3. 是否检查所有 block 从 entry 可达？
   - 是；不可达 block 报 verifier warning。

4. 是否检查所有非-void function 路径 return？
   - 当前必须保证所有路径都有return。

## 12. Debug Print 与测试

1. API 是否采用与 s2/s3 相同的 result 风格？
   - 建议：
     ```cpp
     struct CFGResult {
         optional<CFGProgram> program;
         optional<Error> error;
         vector<Warning> warnings;
         string debug_text;
     };
     ```

2. 是否需要 `buildCFGProgram` 和 `buildCFGProgramOrThrow` 两套 API？
   - 是，保持阶段 API 一致。

3. debug print 应展示哪些信息？
   - 建议：
     - function kind/name
     - entry/exit block
     - 每个 block 的 stmts
     - terminator
     - labeled successors/predecessors

4. 测试目录放哪里？
   - 建议：`testv2/fixtures/s4cfg` 和 `testv2/s4cfg_test.cpp`。

5. 最小测试应覆盖哪些？
   - 建议至少覆盖：
     - straight-line block
     - statement-level call 保留在 block 内
     - if/else branch + merge
     - if 分支 return 不连 merge
     - while condition prelude 在 header block
     - for init/cond/step/body/back-edge
     - do while condition block
     - break/continue targets
     - switch case/default/break
     - top/helper/lambda 全部生成 CFG
     - verifier 拒绝 block 内残留控制流 statement

## 13. 与后续阶段的接口

1. `LoopLowerOrUnroll` 期望看到结构化 loop metadata，还是只看普通 CFG back-edge？
   - 后续需要静态 unroll，s4 需要在 loop header/body/step/exit block 上保留 loop region metadata。

2. `LowerFunctionExits` 期望 `Return` terminator 直接携带 return operand 吗？
   - 是。

3. `InlineCallsCFG` 期望 `CallStmt` result/args/target 如何查找？
   - block 内 `CallStmt` 原样保留。

4. `BuildSSA` 是否需要 phi placement 的完整 predecessor 信息？
   - 是，s4 必须维护准确 predecessor/successor。

5. `PredicateLowering` 是否依赖 edge label？
   - 保留 edge label，降低时可直接构造 path predicate。
