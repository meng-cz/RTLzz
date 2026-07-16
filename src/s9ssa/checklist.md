# S9 SSA Checklist

本阶段目标为：输入 S8 OperationNormalize 之后的 scalar CFG，对变量写入做 SSA 版本化，在控制流合流点插入/表示 phi 或等价 merge，形成每个变量版本和控制流合流点的明确数据依赖；同时将 S8 中仍保留的 `LookupWrite` 拆分为逐元素的 `Mux`/比较/赋值，使后续 PredicateLowering 不再需要理解动态数组写语义。

## A. 阶段边界与输入输出

1. S9 的输入是否固定为 `s8opnorm::S8NormProgram`？
   - 是。

2. S9 是否应重新定义一套独立的 `S9SSAProgram`，而不是复用旧 `src/ir/SSA.*`？
   - 是。旧 SSA 基于字符串变量和旧 AST/CFG 语义；S9 应基于 S8 的 `SymbolId`、`S8Type`、`S8Operation` 与 scalar CFG 重新定义最小 IR。

3. S9 输出是否仍保留 CFG block/terminator 结构？
   - 是。PredicateLowering 需要 CFG guard 信息；S9 只做数据依赖显式化，不消除控制流。

4. S9 是否允许输出中继续存在 `Lookup`？
   - 允许。`Lookup` 是动态读 mux 的抽象，语义上无写副作用；可在 S9 中视作普通 op。

5. S9 是否允许输出中继续存在 `LookupWrite`？
   - 不允许。明确“此处需要拆分 lookupwrite 到逐元素 mux”，S9 输出应彻底移除 `LookupWrite`。

6. S9 是否应生成 debug print 与测试入口？
   - 是。延续 S7/S8 风格，提供 `buildSSA`, `buildSSAOrThrow`, `verifySSAProgram`, `debugPrint`，并在 `testv2` 做从测试。

## B. SSA 标识与版本模型

7. SSA value 应如何命名？
   - 使用 `{base_symbol: SymbolId, version: int}` 作为语义标识，debug print 显示为 `name#base.vversion` 或 `name_vN`。不要用字符串作为语义 key。

8. 每个 S8 symbol 是否都需要 SSA 版本？
   - 所有可被读写的 scalar symbol 都版本化，包括 port、local、temp。literal 不版本化。

9. 输入端口的初始版本如何表示？
   - 显式记录输入端口变量的assign类型是 port。

10. output/mutable-ref 端口的初始版本如何表示？
    - 不允许读output端口的初始版本。

11. local/temp 未赋值前读取如何处理？
    - 报错，不生成 `undef`。除 port initial value 之外，所有读都必须能追溯到 dominating def 或 phi。

12. 是否保留原始 symbol table？
    - 保留 `base_symbols` 作为 debug/type/port metadata；SSA 语义读取使用 SSA value id，不再直接读取 mutable base symbol 当前值。

13. 是否为每个 SSA version 建立全局 dense value id？
    - 是。`S9ValueId` 便于后续 predicate/backend 引用；同时保留 `{base_symbol, version}` 做 debug 与验证。

14. statement target 是否必须是新的 SSA value？
    - 是。任何 `Assign/Op/Lookup/lowered LookupWrite element assignment/Phi` 的结果都产生一个新的 SSA value，不允许覆盖已有 SSA value。

## C. Phi / Merge 表示

15. 合流点使用显式 `Phi` 节点还是立即转换成 `Mux/Ite`？
    - 使用显式 `Phi`。PredicateLowering 再根据 CFG guard 将 phi 转换成 guarded assignment 或 ITE；S9 不提前谓词化。

16. Phi 放在 block 内的哪个位置？
    - 每个 block 开头拥有 `phis` 列表，先于普通 statements。phi 的 incoming 以 predecessor block id 显式标注。

17. Phi 是否只在变量有多个可达 incoming version 且 incoming 不完全相同时插入？
    - 是。若所有 incoming version 相同，不生成 phi，直接沿用该 version。

18. Phi 是否需要包含所有 predecessor 的 incoming？
    - 是。每个可达 predecessor 都必须有一个 incoming；不可达 predecessor 应先在 CFG reachability 中剔除或在 S9 忽略。

19. 如果某个 predecessor 上变量未定义，但其他 predecessor 定义了变量，怎么办？
    - 报错，除非该变量是 port。不要用隐式 undef phi。

20. Exit block 是否需要为 output 端口生成 final phi？
    - 如果 exit 有多个 predecessor，正常 SSA phi placement 会为被多路径定义的 output 生成 phi。额外可在 `S9SSAProgram` 中记录每个 output port 的 final SSA value，方便后续阶段。

21. Switch 多路合流如何处理？
    - 与普通多 predecessor phi 一致，incoming 按 predecessor block id 而不是 case value 绑定；case guard 留给 PredicateLowering 从 CFG 计算。

22. 是否需要 pruned SSA？
    - 实现 pruned 或 semi-pruned SSA，只给 live-in 到合流点的变量插 phi，避免大量无用 phi。若实现复杂，可先 conservative phi，再做 trivial phi 删除；但推荐至少做 live-in 分析。

23. 是否需要处理循环 phi？
    - S5 当前要求循环完全 unroll，S9 可以拒绝存在 back-edge 的 CFG。若未来支持循环，另立语义。

## D. CFG 与支配关系

24. S9 是否重新计算 predecessor/successor？
    - 是。从 S8 terminator 统一派生 CFG edges，不依赖前一阶段可能存在的辅助 predecessor metadata。

25. 是否先移除 unreachable block？
    - S9 不必重写 CFG 删除 block，但构建 SSA 时只处理从 entry 可达的 block；debug/verify 应标记或忽略不可达 block。

26. Dominator/IDF 算法如何选择？
    - 先实现简单稳定算法即可：可达 block 集合上迭代求 dominator，再计算 dominance frontier；当前 CFG 规模预期较小。

27. block 顺序是否保持 S8 原顺序？
    - 保持原 block id 和顺序，便于 debug diff；只在内部使用 reverse-postorder 做分析。

28. `Exit` terminator 是否有 successor？
    - 没有。exit block 本身可作为普通 block；terminator `Exit` 表示函数结束。

29. `Unreachable` terminator 如何处理？
    - 可达 block 中出现 `Unreachable` terminator 表示无 successor；若其中还有使用未定义值仍需验证 statements。不可达 block 可忽略。

## E. Statement 重写规则

30. `Assign target = value` 如何 SSA 化？
    - 先把 RHS operand 重写为当前 SSA value/literal，再给 target base symbol 创建新 SSA value，输出 `Assign new_target = rhs_ssa`。

31. `Op target = op(operands...)` 如何 SSA 化？
    - 所有 operand 重写为 SSA operand，target 创建新 SSA value，operation kind/meta 原样保留。

32. `Lookup target = lookup(index, elements...)` 如何 SSA 化？
    - index 和 elements 都读当前 SSA value，target 创建新 SSA value，保留 `Lookup` statement。

33. Terminator condition/switch value 如何 SSA 化？
    - 按当前环境重写为 SSA operand；terminator 不创建新 value。

34. statement 顺序内多次写同一 base symbol 如何处理？
    - 每次写创建递增版本；后续 statement 读取最新版本。

35. S8 inserted casts/temp 是否继续作为普通 symbol/version 处理？
    - 是。不要在 S9 重新折叠 cast/temp。

36. S8 literal 的 signed metadata 是否保留？
    - 保留在 operand 上。S9 不改变整数语义。

## F. LookupWrite 拆分为逐元素 Mux

37. `LookupWrite(index, value, elem0, elem1, ...) -> targets[]` 的目标语义是什么？
    - 对每个元素 i 生成 `targets[i] = (index == i) ? value_cast_to_elem_i : elem_i`。这表示动态数组写后每个 leaf 的新值。

38. index 与元素编号比较的宽度如何选择？
    - 使用 index 的当前 S8/S9 type width；元素编号 literal 构造为同宽无符号 literal，比较 op 为 `Eq`，结果 bool。

39. `lookup_value` 宽度与单个 target 宽度不同怎么办？
    - N/A，S8 已把 value/elements 提升到最大宽度。

40. 拆分出的比较和 mux 是否生成新 SSA temp？
    - 是。每个 element 生成 `eq_temp_i = Eq(index, literal_i)`，再生成 `mux_temp_i = Mux(eq_temp_i, value_i, old_elem_i)`，最后定义 target 的新 SSA version。也可以让 target 直接作为 mux 结果，减少一个 temp；推荐 target 直接接 mux 结果，eq 仍为 temp。

41. 如果 `lookup_write_targets.size() != lookup_elements.size()` 怎么办？
    - 报错。这是 S8/S7 invariant，不应在 S9 容忍。

42. 如果 index 超出元素范围，语义是什么？
    - 所有元素保持原值，因为没有任何 `index == i` 为 true。这与 mux-per-element lowering 自然一致。

43. 多级动态索引经 S7/S8 可能形成连续 `LookupWrite` 临时，S9 是否需要特别识别？
    - 不需要。按 statement 顺序逐个 lowering，每个 LookupWrite 的 `lookup_elements` 读当前 SSA version 即可。

44. `LookupWrite` lowering 是否应在 SSA renaming 前还是过程中做？
    - 在 SSA renaming 过程中处理。因为它既读取旧元素又写回多个 target，必须使用同一语句进入前的环境读取 all old elements，然后再依次创建 target 新版本，避免前一个 target 更新影响后一个 target 的 old element。

45. 同一个 `LookupWrite` 的 `lookup_write_targets` 是否允许重复 symbol？
    - 不允许，报错。逐元素数组 leaf 应唯一。

46. `LookupWrite` 拆分后 S9 输出是否需要保留来源 metadata？
    - 保留 debug loc 和 source statement kind/hint，debug print 中标出 `lowered_lookupwrite`，便于测试与诊断。

## G. Types 与 Operand 表示

47. S9 是否复用 `S8Type/S8Literal/S8OpKind`？
    - 不复用，定义自己的结构。

48. S9 operand 是否应区分 Literal 与 SSAValue？
    - 是。不要再允许 operand 直接引用 base `SymbolId`。

49. S9 statement target 是否应使用 `S9ValueId` 而非 `SymbolId`？
    - 是。base symbol 只用于说明这个 value 属于哪个原始 scalar symbol。

50. Phi result 的 type 如何决定？
    - 等于 base symbol type；所有 incoming value type 必须完全一致，否则报错。

51. S9 是否允许 signed_view 在同一 SSA value 的不同 use 上不同？
    - 允许。signedness 是 operand view，不是 value/signal 属性；SSA value 本身只记录 width/kind。

## H. Ports、返回值与程序出口

52. S9 输出如何标记 top ports？
    - 保留 S8 ports，但 port symbol 映射到 base symbol；另记录每个 port 的 initial value 与 final value。

53. input port 是否允许被写？
    - 不允许。

54. output port 是否必须在所有路径上有 final value？
    - 是。若 output 的 final SSA value 在 exit 处未定义且没有合法 initial v0，报错。对于 mutable-ref，v0 可作为保留原值；对于纯 output 不允许未写保持原值。

55. 是否在 S9 就检查输出覆盖完整？
    - 做基础检查：final output value 必须存在；更复杂的 guard 覆盖/多驱动冲突留给 PredicateVerifyAndSimplify。

## I. 错误处理与验证

56. 错误报告使用哪套机制？
    - 使用 `RTLZZException` / `ErrorContext`，stage 填 `s9ssa`，包含 block id、statement index、symbol/debug name 等 note。

57. `verifySSAProgram` 应检查哪些不变量？
    - dense value ids；每个 non-initial value 有唯一 def；每个 operand 引用已存在 value；phi incoming 覆盖所有可达 predecessors；类型一致；输出无 `LookupWrite`；terminator operand 为 bool/switch value 类型合法。

58. SSA 是否需要检查 def dominates use？
    - 是。普通 statement use 由 renaming 保证，verify 仍应在 CFG 上检查；phi incoming 使用 predecessor 末尾版本，不要求 def dominate phi block 内普通语义，但需 dominate对应 predecessor end。

59. 遇到 S8 中残留 loop/back-edge 怎么办？
    - 报错，提示 S5 unroll 未完成。S9 第一版不支持循环 SSA。

60. 遇到 malformed CFG（非法 block id、缺失 successor、exit 不可达）怎么办？
    - 报错，不尝试修复。

## J. Debug Print 与测试需求

61. debug print 应显示哪些内容？
    - symbols/ports、initial values、每个 block 的 phi、SSA statements、terminator、output final values、summary（phi 数、value 数、lowered lookupwrite 数）。

62. 单元测试需要覆盖哪些手写 S8 场景？
    - 直线赋值版本递增；if/else phi；只有一个分支写变量的报错/port v0 合流；switch 多路 phi；unreachable block；lookup read；lookupwrite 拆 mux；同一 block 多次写同 symbol。

63. 集成测试需要覆盖哪些 cpp 到 S9 场景？
    - 简单分支输出、nested branch、switch、动态数组写、嵌套动态数组写经 S7/S8 后到 S9、output mutable-ref 读旧值。

64. 是否需要 golden debug string 测试？
    - 需要，但只断言关键片段，避免 block/temp 命名微调造成脆弱测试。

65. S9 是否应加入 CMake 自动目标？
    - 是。沿用当前 testv2 自动发现 cpp 的方式。

68. S9 输出是否应作为 PredicateLowering 的唯一输入？
    - 是。
