# s6inline 语义澄清清单

本阶段目标为：在 `s5unroll` 之后，于 CFG 层内联 helper/lambda 调用。具体做法是 clone callee CFG、绑定实参与形参、为 callee 局部/临时/return slot 分配 caller function 内新的 `SymbolId`、将 callee return/exit 连接到 caller continuation，并最终让后续阶段不再面对 helper/lambda 的 statement-level call。

下面问题需要先明确，确认后再开始编码。

## 1. 阶段边界

1. `s6inline` 的输入是否固定为 `s4cfg::CFGProgram`，且语义上要求已经经过 `s5unroll`？
   - 是。输入仍复用 S4 CFG 数据结构，但 verifier 需要确认所有 function 已经 `loop_regions.empty()`，S6 不处理 loop。

2. 输出是否继续使用 `s4cfg::CFGProgram`，还是定义新的 `InlinedCFGProgram`？
   - S6 定义全新的 InlinedCFGProgram，进一步收窄数据结构，不再允许包含 helper/lambda call、loop region、loop stack等已经被S5/S6处理的信息。

3. S6 是否只处理 helper/lambda call，不处理 `ConstructStmt`、硬件构造、aggregate lowering、predicate lowering？
   - 是。`ConstructStmt` 和 aggregate 操作继续保留给后续阶段。

4. S6 完成后，输出中是否允许残留任何 helper/lambda `CallStmt`？
   - 不允许。

5. S6 是否负责删除 `program.helpers` / `program.lambdas`，还是保留作为 debug metadata？
   - top 与 helper/lambda 的函数体本身都应已内联完毕；清空，避免后续阶段误用。

6. 错误是否继续使用 `RTLZZException` / `ErrorContextGuard`？
   - 是，阶段名固定为 `s6inline`。

## 2. Inline 范围与顺序

1. 是否处理 top、helpers、lambdas 中的全部 call graph，而不仅是 top？
   - 是。递归把 helper/lambda 自身内部 call 也内联，再把它们被 clone 到 caller，保证任意输出 function 都没有 helper/lambda call。

2. inline 顺序是否采用 call graph 后序/topological order？
   - 是。S2 已禁止递归；S6 可再验证无递归并按 callee-before-caller 处理。

3. 若 helper A 调 helper B，top 调 A，是否应先生成“已内联 B 的 A”，再把 A clone 到 top？
   - 是，避免 clone 后再次在 caller 内扫描跨层 call。

4. lambda 与 helper 是否统一处理？
   - 是。差别只在 lookup 表和 debug name 前缀。

5. 如果同名 helper 与 lambda 同时存在，call resolution 规则是什么？
   - 按照C++标准，lambda 优先。

## 3. Call Resolution

1. `CallStmt::callee` 如何匹配 helper/lambda？
   - 按照函数签名匹配，允许重载，当无法判断重载时报错。

2. 未解析到 helper/lambda 的 `CallStmt` 如何处理？
   - 直接报错。允许的内置函数已经在AST中被解析。

3. `Int`、`UInt`、`bool`、`Int<N>`、`UInt<N>` 这类硬件构造如果仍以 `CallStmt` 出现，是否不由 S6 处理？
   - 已经被消除掉，不会以call形态出现。

4. 是否允许函数指针、虚调用、方法调用、operator call 在 S6 出现？
   - 不允许，S2/S3 应已消除或拒绝；S6 发现无法解析的普通 call 报错。

5. inline 之后是否需要记录 `InlineSummary{caller, callee, call block, cloned blocks}`？
   - 需要，供 debug print 与测试断言使用。

## 4. 参数绑定

1. Value 参数如何绑定？
   - 为 callee 形参 symbol 分配 caller 内 fresh symbol，插入 `Decl param_clone` 和 `Assign param_clone = actual`，callee body 内读写都指向 param_clone。

2. ConstRef 参数如何绑定？
   - 同value，直接复制。

3. MutableRef / Output 参数如何绑定？
   - 把 callee 形参 symbol remap 到 caller actual lvalue/root symbol，使 callee 内写入直接作用到 caller 变量；若 actual 不是可写简单 lvalue，则报错。

4. Pointer / RValueRef 是否应在 S6 出现
   - 不应出现；S2 应拒绝，S6 再防御性报错。

5. aggregate、array、struct 参数是否在 S6 按整体 symbol 绑定，还是需要先 flatten？
   - 按当前 S3/S4 lvalue/operand 结构做整体绑定，不展开 aggregate；字段/数组访问仍交给后续 flatten/lvalue lowering。

6. 形参与实参数量、类型、方向是否在 S6 再校验？
   - 做防御性校验，错误信息附 call site context；S2 是主校验阶段，但 S6 不信任输入。

7. 对于 value 参数的初始化语句，应插入在 call 所在 block 中，还是插入到 cloned callee entry 前的 synthetic binding block？
   - 创建 binding block，先执行参数 decl/assign，再跳到 cloned callee entry，便于 CFG splice 清晰。

8. 对于 const-ref/mutable-ref 参数，如果 actual 是带 field/index access 的 lvalue，是否允许直接 remap 成复杂 lvalue？
   - 允许，需要在内联时直接对callee内部符号进行替换。

## 5. Return Value

1. callee 已经经过 S4 `lowerFunctionExits`，是否统一从 `callee.return_slot_symbol` 读取返回值？
   - 是。non-void callee clone 后读取 cloned return slot。

2. call 有 `call_result` 且 callee non-void 时，如何写回 caller？
   - 在 inline continuation 前插入 `Assign call_result = cloned_return_slot`。

3. call 没有 `call_result` 但 callee non-void，是否允许丢弃返回值？
   - C++ 允许表达式语句丢弃返回值；推荐允许，并不生成写回。

4. call 有 `call_result` 但 callee void，是否报错？
   - 报错。

5. void callee 的 return/exit 如何连接？
   - 所有 cloned return-to-exit 路径最终跳到 caller continuation，不生成 return value assignment。

6. callee 多 return 的语义是否已经由 S4 return slot 合一解决？
   - 是。S6 只需要把 cloned callee exit 接到 continuation。

## 6. CFG Splice 形态

1. call 位于 basic block 中间时，是否需要 split caller block？
   - 是。call 前语句留在原 block，call 后语句移动到 continuation block。

2. 一个 block 中有多个 call 时如何处理？
   - 逐个处理，每次 split 后从 continuation 继续扫描，保持原求值顺序。

3. cloned callee entry/exit 是否保留原样？
   - clone callee blocks，但不要保留 callee 原 function exit 的 `Exit` 终止语义；clone exit 改成 jump 到 caller continuation。

4. call statement 本身是否完全删除？
   - 是，由 binding block、cloned callee blocks、return writeback block 替代。

5. caller 原 block 的 predecessors/successors 是否完全重建？
   - 是。每次 splice 后重建或严格维护 edge/predecessor/successor 一致性。

6. inline 后是否允许产生空 jump block？
   - 允许，后续可优化；S6 verifier 只要求 CFG 合法。

7. cloned callee 中如果有 `TermKind::Return` 残留，是否允许？
   - 不应残留。S4 lower 后应统一 return 到 exit；S6 verifier 可拒绝非 exit-oriented return。

8. cloned callee 中如果有 `loop_stack` 或 `loop_regions`，是否报错？
   - 报错，S6 输入必须是 S5 后 loop-free CFG。

## 7. SymbolId 与重命名

1. S6 是否必须保持 caller function 内 `SymbolId` 唯一？
   - 所有 cloned callee symbols、value-param clones、return slot clones、synthetic temps 都必须分配 caller function 内 fresh id。

2. callee 的局部变量、临时变量、return slot 是否全部 fresh clone？
   - 是，除非形参被明确 remap 到 caller actual。

3. callee 形参在不同 passing kind 下是否可能不 fresh clone？
   - ConstRef/Value 参数 fresh clone；MutableRef 可 remap 到 caller actual。

4. cloned symbol name 如何生成？
   - `__s6_<callee>_<call_index>_<old_name>` 加唯一后缀；name 仅用于 debug，不作为身份。

5. cloned block id 如何分配？
   - 在 caller function 内从最大block id 递增分配。

6. cloned `LValue`、`Operand`、`Terminator`、`SwitchTarget` 中所有 symbol 引用是否都必须经过 remap？
   - 必须。

7. S3/source scope metadata 如何处理？
   - cloned/synthetic symbols 的 `declaring_scope = -1`，`source_valid_scope_ids` 为空；S6 不依赖 scope metadata。

8. 如果 callee symbol id 不是 0..N-1 或不唯一，S6 是否报错？
   - 报错，延续 post-S3 invariant。

## 8. 控制流与异常情况

1. callee 内是否允许 CFG branch/switch？
   - 允许，S6 clone 整个 CFG 并保持其内部控制流。

2. callee 内是否允许 unreachable block？
   - 允许但 warning；clone 前后 verifier 仍需保证 target 合法。

3. callee 内是否允许 residual `EdgeKind::Break/Continue`？
   - 不允许，因为 S5 后 loop-free，switch break 应已不是 loop-control residual。

4. callee 内是否允许 residual helper/lambda call？
   - 内联顺序应先消除；若 clone 时仍发现，报错或递归 inline，需明确。

5. S6 是否需要处理 early-return 的 active flag？
   - 不需要。S4 return slot + unified exit 已经把 early-return 内化为 CFG return-to-exit，S6 只连接 callee exit 到 continuation。

6. 如果 callee 可能从多条路径到 exit，return writeback 应放在哪里？
   - 放在 cloned callee exit 之后的 single writeback block，再跳 continuation。

## 9. Program 级输出

1. 输出 program 中是否只保证 `top` fully inlined，还是 helpers/lambdas 也 fully inlined？
   - S6不再输出除了主函数之外的其他函数。

2. 后续阶段是否只消费 `program.top`？
   - 是，S6不应再包含其他输出。

3. `helper_index` / `lambda_index` inline 后是否保留？
   - 清空。

4. struct metadata 是否原样透传？
   - 是，S6 不处理 struct/array flatten。

## 10. Verifier

1. S6 输出 verifier 至少应检查哪些？
   - 推荐：
     - 所有 block id 与 vector index 一致
     - entry/exit target 合法
     - predecessors/successors 与 terminator 一致
     - 所有 symbol id function-local unique 且 id 等于 symbols index
     - 所有 symbol 引用指向本 function symbols
     - no loop regions / no loop stack
     - no helper/lambda call residual
     - call_result 与 callee return type 一致
     - non-void function return slot 合法

2. 是否检查 CFG acyclic？
   - 是，因为 S5 后应已无 loop，S6 clone 不应引入 cycle。

3. 是否检查 cloned callee block 不再引用 callee-only symbol id？
   - 必须。

4. 是否检查 const-ref 参数未被写？
   -  S2 已检查，但 S6 可通过 cloned callee writes to param symbol 做防御性检查。

## 11. API、Options 与 Debug Print

1. API 是否采用现有阶段风格？
   - 推荐：
     ```cpp
     struct InlineResult {
         std::optional<s4cfg::CFGProgram> program;
         std::optional<InlineError> error;
         std::vector<InlineWarning> warnings;
         std::vector<InlineSummary> summaries;
         std::string debug_text;
     };
     ```

2. 是否需要 `inlineCFGProgram` 和 `inlineCFGProgramOrThrow` 两套 API？
   - 是。

3. Options 需要哪些字段？
   - 推荐：
     - `debug_print`
     - `max_inline_depth`
     - `max_cloned_blocks`
     - `keep_callee_metadata`
     - `allow_unresolved_calls`

4. Debug print 打印什么？
   - 推荐：
     - 每个 inline site：caller、callee、原 block、call index、cloned block range
     - 参数绑定表：callee symbol -> caller symbol / cloned symbol
     - return slot 写回
     - 最终 CFG 使用 S4 debug print

5. warning 用于哪些情况？
   - 推荐：
     - 丢弃 non-void 返回值
     - clone unreachable callee block
     - 保留允许名单内 unresolved external call
     - 生成空 continuation block

## 12. 测试范围

1. 测试目录放哪里？
   - 推荐：`testv2/s6inline_test.cpp`、`testv2/s6inline_integration_test.cpp`，fixtures 放在 `testv2/fixtures/s6inline`。

2. 最小正向单元测试应覆盖哪些？
   - helper value 参数 inline
   - void helper inline
   - non-void helper return slot 写回
   - call 位于 block 中间时 split continuation
   - 一个 block 内多个 call 顺序 inline
   - callee 内 if/switch CFG 被完整 clone
   - callee local/temp/return slot symbol fresh clone
   - helper 调 helper 的 topological inline
   - lambda inline

3. 参数语义测试应覆盖哪些？
   - value 参数写入不影响 caller actual
   - const-ref 参数读 caller actual
   - mutable-ref/output 参数写回 caller actual
   - 非简单 lvalue 作为 mutable-ref/output actual 的错误

4. 负向测试应覆盖哪些？
   - unknown call
   - recursive call graph residual
   - argument count mismatch
   - return type / call_result mismatch
   - callee 含 loop region
   - cloned symbol id 冲突
   - unresolved helper/lambda call residual

5. 源级 integration test 应覆盖哪些？
   - C++ source -> S3 -> S4 -> S5 -> S6 的完整路径
   - top/helper/lambda 全部无 loop、无 helper/lambda call residual
   - nested helper calls
   - branch 中的 call
   - loop 展开后产生的多份 call 被 S6 继续内联

