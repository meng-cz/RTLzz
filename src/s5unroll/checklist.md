# s5unroll 语义澄清清单

本阶段目标为：在 `s4cfg` 之后，对静态可展开循环进行展开；当前硬件子集可先要求所有循环在此阶段完全 unroll。输入是 per-function CFG program，输出应是仍然保持 CFG 形态、但不再包含 loop back-edge / loop region 的 program。后续 CFG inline、alias/param lowering、aggregate flatten、SSA、predicate lowering 只面对无循环 CFG。

下面问题需要先明确，确认后再开始编码。

## 1. 阶段边界

1. `s5unroll` 的输入是否是 `s4cfg::CFGProgram`？
   - 是，严格接在 `BuildFunctionCFGs` 后，不回退到 `FunctionAST` 或 `StatementizedProgram`。

2. 输出是否继续使用 `s4cfg::CFGProgram`，还是定义新的 `UnrolledCFGProgram`？
   - 定义 `src/s5unroll` 自己的 result/API，但 program 数据可以复用或包装 s4 的 `CFGProgram`。

3. 本阶段是否处理 top/helper/lambda 全部 function CFG？
   - 是，所有 function 内的 loop 必须完全展开。

4. 本阶段是否允许输入 CFG 中还有 loop 但输出中没有 loop？
   - 是；输出 verifier 必须确认 `loop_regions.empty()` 且 CFG 中无环

5. 本阶段是否做 inline、return slot、alias lowering、aggregate flatten 或 predicate lowering？
   - 都不做；只处理 loop unroll 与由 break/continue 引入的必要控制流改写。

6. 错误是否继续使用 `RTLZZException` / `ErrorContextGuard`？
   - 是，阶段名固定为 `s5unroll`。

## 2. 静态可展开循环的定义

1. 当前阶段是否只支持 canonical `for` loop？
   - 只支持可以被证明常量次循环的for。

2. 是否必须支持 `while` / `do while` 的静态展开？
   - 只支持可以被证明常量次循环（或常量次循环上限）的 while/do while。

3. canonical `for` 的可识别来源是什么？
   - 源于整个循环内对于循环变量的初始赋值、修改、条件判断的静态分析。

4. 如果 s4 metadata 不足，是否允许修改 s4 数据结构以保留原始 S3 loop 指针或 canonical loop info？
   - 暂时不允许，如果存在无法分析识别但应当静态可展开的情况，请在实现报告中向我说明。

5. 支持哪些 induction variable 类型？
     - builtin integer only
     - `Int<N>` / `UInt<N>`
     - bool 不允许
     - enum 不允许

6. 支持哪些 init 形式？
     - `int i = literal`
     - `uint32_t i = literal`
     - `Int<N> i = Int<N>(literal)`
     - `i = literal`
   - 允许 init 在 loop 外。

7. 支持哪些 condition 形式？
     - 各种比较运算符
   - 不允许 condition prelude 中包含 call/side effect。

8. 支持哪些 step 形式？
   各种二元运算符的 i = i op const or const op i

9. loop bound / step 是否必须是 literal 常量？
   - 是。

10. 最大展开次数如何限制？
   - 提供 `UnrollOptions::max_iterations`，默认例如 1024；超过时报错，由s5 API 参数指定。

11. 零次迭代如何处理？
   - 允许，输出直接连接 loop 前驱到 loop exit，并保留 init 的副作用。

12. 负 step / 递减 loop 是否允许？
   - 可静态分析出循环次数即可。

13. 溢出语义如何处理？
   - 按照循环变量自身的类型语义处理（所有Int与内置整数均采用回绕式溢出）。

## 3. CFG 展开策略

1. 展开应在 CFG 层 clone loop body region，还是转回结构化 S3 再重建 CFG？
   - 在 CFG 层 clone loop region，并删除原 loop region/back-edge。

2. 如何确定 loop region 内部包含哪些 blocks？
     - 使用 `LoopRegion` 的 header/body/step/condition/exit 和 `loop_stack`

3. clone 后 block id 如何分配？
   - 接着原来的最大值继续分配递增 block id。

4. predecessor/successor 是否完全重建？
   - 是，避免 clone 后 edge 残留。

5. scope id 如何处理？
   - clone blocks 保留原 scope stack，每次迭代需要独立 scope id，接着原来的最大值递增分配。

6. loop region metadata 如何处理？
   - 展开后的对应 loop region 删除；若 body 内有 nested loop，递归展开后最终 `loop_regions` 为空。

7. return slot / Return terminator 是否保持原样？
   - 保持；s5 不再改 return value slot。

8. Switch terminator 是否保持原样？
   - 保持；但 switch 内的 break 已由 s4 指向 switch exit，与 loop break edge 不同，s5 必须不要误处理。

## 4. 变量替换与每轮迭代常量化

1. 是否需要把 loop induction variable 在每轮 body 中替换为 literal？
   - 是，否则展开后仍残留变量更新和条件，后续 SSA/constant folding 才能化简。

2. 替换范围是什么？
   - 只替换 body clone 中对 induction variable 的 reads；不替换被重新声明 shadow 的同名局部。

3. 是否需要删除原 loop header condition 和 step statement？
   - 是；展开后不应再保留 loop condition/step 的控制语义。init 是否保留需按 init 形式确认。

4. induction variable 本身是否仍需要存在？
     - 如果 loop variable 是 loop init 内声明的局部，只保留其 Decl 或可完全移除？
     - 如果 variable 是在loop前声明的，则必须保留最终值。

5. 对 `for (int i=0; ... )` 的 `i` 作用域如何处理？
   - 确认 s4 scope metadata 是否足够表达 loop-local `i`，展开后 `i` 应对外不可见。

6. 每轮是否需要生成 per-iteration renamed temporaries？
   - 展开后需要重新分配scope id。

7. 用户局部变量在 loop body 内声明时如何处理？
   - 每轮 clone，每轮分配到独立的scope。

8. 对 assignment target 中的 induction variable 是否替换？
   - 应替换；如果作为 assignment root `i = ...` 是 loop step，其后续可能不会被使用，但仍然替换并保留，后续会优化。

9. loop body 是否允许修改 induction variable？
   - 允许，所有修改都会被统一纳入可静态展开性的分析，如果存在非 canonical 修改则报错（不可静态展开）。

## 5. break / continue 语义

1. 展开后是否允许 CFG 中残留 `break` / `continue` edge kind？
   - 不允许来自已展开 loop 的 `break/continue` edge 残留；switch break edge 可作为 normal jump/fallthrough 语义保留或改 label。

2. 如何表达 `break`？
   - 引入 synthetic boolean + if。

3. 如何表达 `continue`？
   -  continue 跳到本轮 step/end，然后进入下一轮。

4. 是否允许 loop body 内有 break/continue？
   - 支持。

5. break/continue 嵌套在 if/switch 中如何处理？
     - loop break vs switch break 必须区分（s4已区分）
     - loop continue 只影响当前 loop

6. break 后同一迭代剩余语句是否必须不执行？
   - 是。

7. continue 后同一迭代剩余 body 是否必须不执行，但 step 仍执行？
   - 是。


## 6. while / do while 展开

1. 第一版是否拒绝所有 `LoopKind::While/DoWhile`？
   - 明确仅支持 canonical 模式（循环次数可静态分析）。

2. 若支持 while，静态 pattern 是什么？
   - 需要确认，例如：
     - loop 前 `i = init`
     - header condition with constant bound
     - body 固定 constant step

3. condition prelude 中含 call 是否允许？
   - 静态 unroll 不允许 condition call/side effect。

4. do-while 零次/一次语义如何处理？
   - 同for，直接展开空块或展开一次。

## 7. 嵌套循环

1. nested loop 展开顺序是什么？
   - 先递归展开 inner，再展开 outer。

2. 展开计数是否使用乘积检查最大 block/iteration 数？
   - 总 clone block 数 / 总 unrolled iterations 都要受 options 限制。

3. nested loop 中 break/continue 目标如何保持正确？
   - 按照当前递归展开上下文判断。

4. loop id / generated name 如何避免冲突？
   - 每个展开内容中scope id持续递增。

## 8. 函数、helper、lambda

1. top/helper/lambda 是否全部要求 loop-free？
   - 是，任一 function 含不可展开 loop 都报错。

2. helper/lambda 中的 loop 是否在 inline 前展开？
   - 是；需要确认后续 inline 只面对 loop-free callee CFG。

3. helper/lambda 的 generated names 是否函数内唯一即可？
   - 是，后续 inline 再做跨函数 rename 与 binding。

## 9. 与 return/switch/call 的交互

1. loop body 中出现 `Return` terminator 是否允许？
   - 允许，展开后应保持 return edge 到 function exit，并使后续 iterations 不执行；这需要 active/returned flag 或 CFG control routing。

2. 第一版是否禁止 loop body 内 return？
   - 允许。

3. loop body 中 `CallStmt` 是否原样 clone？
   - 是；每轮 call 都保留为独立 statement-level call。

4. loop body 中 `ConstructStmt` / aggregate copy 是否原样 clone？
   - 是，后续 aggregate lowering 再处理。

5. switch inside loop 是否支持？
   - 若 switch 内只含普通 break 到 switch exit，应可 clone；若含 loop break/continue，需要同正常break/continue处理。

## 10. 输出 CFG 约束与 verifier

1. 输出是否必须完全无 loop region？
   - 是。

2. 输出是否必须无循环 back-edge / CFG cycle？
   - 是。

3. 是否要做 graph acyclicity 检查？
   - 是；除了 function return edge 到 exit，不应有 cycle。

4. 是否允许 edge label `backedge`、`continue`、`break` 残留？
   - 输出中不保留 loop 的 `backedge/continue/break` label；switch break 应当为 `jump` 或 `switch_break`。

5. verifier 需要检查哪些？
   - 建议至少：
     - 所有 function `loop_regions.empty()`
     - block id / edge predecessor/successor 一致
     - no back-edge/cycle
     - no loop-control edge from unrolled loops
     - all terminator targets valid
     - entry/exit valid
     - non-void function return slot 仍存在
     - generated decl/use 名字不冲突

## 11. API、Options 与 Debug Print

1. API 是否采用与 s2/s3/s4 相同 result 风格？
   - 建议：
     ```cpp
     struct UnrollResult {
         optional<s4cfg::CFGProgram> program;
         optional<Error> error;
         vector<Warning> warnings;
         string debug_text;
     };
     ```

2. 是否需要 `unrollCFGProgram` 和 `unrollCFGProgramOrThrow` 两套 API？
   - 是。

3. Options 需要哪些字段？
   - 建议：
     - `debug_print`
     - `max_iterations_per_loop`
     - `max_total_cloned_blocks`
     - `allow_while`
     - `allow_loop_control`

4. debug print 打印什么？
   - 复用 s4 debug print：
     - function name
     - loop id/kind
     - iteration count
     - generated block range
     - rejected reason/warnings

5. warning 用于哪些情况？
     - 删除 unreachable loop exit/header remnants
     - zero-iteration loop
     - removed unused loop-local decl

## 12. 测试范围

1. 测试目录放哪里？
   - 建议：`testv2/fixtures/s5unroll` 和 `testv2/s5unroll_test.cpp`，必要时再加源级 integration test。

2. 最小正向测试应覆盖哪些？
   - 建议：
     - `for (int i=0; i<4; ++i)` 展开为 4 份 body
     - `for` zero iteration
     - `for` 递减 loop
     - loop body 中 array index 使用 `i` 被替换为 literal
     - loop body 中 statement-level call 被 clone 多份
     - top/helper/lambda 全部展开
     - nested for 展开

3. 最小负向测试应覆盖哪些？
   - 建议：
     - 非常量 bound
     - 非常量 step
     - unsupported while/do-while
     - iteration count 超限
     - loop body 修改 induction variable
     - loop body return，如果第一版不支持
     - break/continue，如果第一版不支持

4. 如果决定支持 break/continue，正向测试还应覆盖：
   - `if (...) continue`
   - `if (...) break`
   - switch 内 break 不退出 loop
   - nested loop continue/break 只作用于内层
