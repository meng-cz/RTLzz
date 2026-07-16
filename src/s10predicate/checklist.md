# S10 PredicateLowering Checklist

本阶段目标为：输入 S9 SSA CFG，将控制流 lowering 为 predicate-friendly 的无 CFG 中间表示。输出应显式表达每个 SSA value/port final value 的数据依赖和 guard 条件，不再保留 basic block、terminator、phi、branch/switch 控制流结构；后续 S17 PredicateVerifyAndSimplify 负责覆盖性、多驱动、宽度一致性和基础 guard/表达式化简。

## A. 阶段边界与输入输出

1. S10 的输入是否固定为 `s9ssa::S9SSAProgram`？
   - 是。S10 不再接收 S8/S7/旧 SSA，确保输入已经是 scalar、normalized operation、SSA value 语义，并且不再存在 `LookupWrite`。

2. S10 是否重新定义独立的 `S10PredicateProgram`，不复用旧 `predicate/PredicateIR.h`？
   - 是。旧 `PredicateIR` 基于 `ExprPtr`/字符串变量，和 S9 的 `S9ValueId`/normalized op 不匹配。S10 应定义 value-id based、width-aware 的 predicate IR。

3. S10 输出是否保留 CFG block？
   - 不保留。S10 的主要职责就是消除控制流，输出 flat definitions / assignments。

4. S10 输出是否保留 phi？
   - 不保留。所有 phi 必须 lowering 成显式数据依赖，用 `Mux` expression/value。

5. S10 输出是否保留 terminator/branch/switch？
   - 不保留。branch/switch 只用于计算 block guard 和 edge guard，输出中不再出现控制流 terminator。

6. S10 输出是否允许保留 `Lookup`？
   - 允许。S9 已经消除了写副作用的 `LookupWrite`；`Lookup` 是纯动态读，可作为 predicate op/value 保留，后续 backend 或优化阶段再决定是否展开成 mux tree。

7. S10 是否需要生成 debug print 和测试入口？
   - 是。延续 S7-S9 风格，提供 `lowerPredicates`, `lowerPredicatesOrThrow`, `verifyPredicateProgram`, `debugPrint`。

## B. 输出 IR 数据结构

8. Predicate operand 应如何表示？
   - 区分 `Literal` 与 `Value`。`Value` 引用 S10 value id；不允许引用 S9 block-local context 或 base mutable symbol。

9. Predicate value 是否继续使用 dense id？
   - 是。S10 可以复制 S9 的 value 作为 source values，并为 phi/mux/guard 计算等新增 generated values 分配新的 dense id。

10. S10 是否保留 S9 value 的 `{base_symbol, version}` metadata？
    - 保留为 debug metadata。语义上使用 S10 dense value id，base symbol/version 只用于打印和 port final 映射。

11. S10 的 statement/definition 类型有哪些？
    - 每条一个symbol，给出定义和赋值来源，可以源于port, assignment或operation。

12. 普通 SSA statement 是否要变成 guarded definition？
    - 是。block 内每个 statement 的 target value 定义 guard 为该 block 的 block guard。

13. Phi lowering 产物应是 statement 还是 value expression？
    - 生成一个定义 phi result value 的 `Mux` op，guard 为 phi block guard 或 incoming guard union；rhs 是按 incoming edge guard 组成的 mux chain。这样后续所有使用 phi result 的地方仍引用同一个 value id。

14. Port final value 如何表示？
    - 输出 `ports` 保留 base symbol metadata，并显式记录每个 output/mutable-ref port 的 `final_value`。若 final value 是 phi/mux lowering 后的新值，指向该 S10 value。

15. S10 是否应输出 assignment 列表还是 value graph？
    - 直接输出 assignments 列表，也要保持 SSA single-def。

16. 是否在 S10 中引入 `Guard` 专用类型？
    - 使用普通 bool `S10Operand`/`S10ValueId` 表达 guard，另提供 `true` literal guard。这样 guard 与普通 bool 运算共享 op 表示。

## C. Guard 语义

17. entry block 的 guard 是什么？
    - 常量 true。

18. Jump edge 的 guard 如何计算？
    - `edge_guard = block_guard`。

19. Branch true edge 的 guard 如何计算？
    - `edge_guard = block_guard && condition`。

20. Branch false edge 的 guard 如何计算？
    - `edge_guard = block_guard && !condition`。

21. Switch edge guard 如何计算？
    - case edge 为 `block_guard && (switch_value == case_literal)`；default edge 为 `block_guard && !(case0 || case1 || ...)`。如果多个 case 指向同一 block，进入该 successor 的 guard 是这些 edge guard 的 OR。

22. 多 predecessor block 的 block guard 如何计算？
    - `block_guard = OR(all incoming edge guards from reachable predecessors)`。

23. block guard 是否必须按拓扑顺序计算？
    - 是。S9 已拒绝循环，S10 可要求 acyclic reachable CFG。若检测到环，报错。

24. unreachable block 如何处理？
    - 忽略其 statements/phis/terminator，不输出 definition；debug print 可标记 ignored unreachable count。

25. guard 表达式是否需要做布尔化简？
    - S10 只做局部恒等化简：`true && x -> x`、`false && x -> false`、`x || false -> x`、`x || x -> x`、`!!x -> x`。复杂 tautology/BDD 化简留给 S17。

26. guard 表达式是否应 materialize 为 S10 values？
    - 是。除 true/false literal 外，`And/Or/Not/Eq` 等 guard op 生成 S10 generated value，方便后续统一处理和 debug。

27. guard value 的 type 是否必须是 bool width=1？
    - 是。S10 verify 应检查所有 definition guard 均为 bool。

## D. Phi Lowering

28. Phi 应该转换成 guarded assignments 还是 mux chain？
    - 转换成 mux chain value：`phi = Mux(edge_guard_i, incoming_i, fallback)`，从最后一个 incoming 开始构造嵌套 mux。这样 S10 输出仍是 SSA single-def value graph。

29. phi incoming guard 用 predecessor block guard 还是 exact edge guard？
    - 使用 exact edge guard。对于 switch 多 case 指向同一 successor、或未来复杂 CFG，predecessor block guard 不足以区分进入边。

30. S9 phi incoming 目前只有 predecessor block id，没有 edge id；S10 如何得到 exact edge guard？
    - 对每个 `(pred, succ)` 聚合所有从 pred 到 succ 的 edge guard。若同一 pred 有多条 edge 到同一 succ，使用 OR 合并，作为 phi incoming guard。

31. phi mux chain 的 fallback 如何选择？
    - 最后一个 incoming value 作为 fallback；前面的 incoming 依次 `Mux(incoming_guard_i, incoming_value_i, fallback)`。由于所有 incoming guard 的 OR 应覆盖 block guard，S17 再验证覆盖关系。

32. phi result 的 definition guard 是什么？
    - phi block 的 block guard。若 block guard 为 false/不可达，则不应生成 phi definition。

33. 单 incoming phi 如何处理？
    - 直接 alias/assign phi result = incoming value，guard 为 block guard；或消除该 phi 并替换使用。推荐第一版生成 assign，保持 value id 稳定。

34. trivial phi（所有 incoming 相同）是否可能出现？
    - S9 已尽量不生成；S10 可再次消除或 assign。同样推荐第一版生成 assign 或直接 alias，verify 不应失败。

35. phi incoming type 不一致怎么办？
    - 报错。这是 S9 invariant，不在 S10 插 cast。

## E. Statement Lowering

36. S9 `Assign` 如何 lowering？
    - 生成 target value 的 definition：`target = operand`，guard 为 block guard。

37. S9 `Op` 如何 lowering？
    - 原样复制 op kind/meta/operands，guard 为 block guard；operand 中引用的 S9 value 映射到 S10 value。

38. S9 `Lookup` 如何 lowering？
    - 原样作为 op definition，guard 为 block guard；index/elements 均映射成 S10 operands。

39. S9 generated values 是否都需要 definition？
    - 是。S9 中 `Generated` 只是 value kind metadata，S10 必须为每个 emitted/generated value 保留唯一 definition。

40. 初始 input value 是否需要 definition？
    - 不需要普通 definition，记录为 `port` value kind；guard 可视为 true。

41. 未被任何 output/final/use 依赖的 value 是否输出？
    - 第一版保留所有 reachable block 中的 definitions，DCE 留给后续优化。debug/test 更容易定位阶段行为。

42. statement 的 debug loc 如何传递？
    - 保留 S9 statement/debug loc；由 guard lowering 生成的 bool ops 继承 branch/switch terminator loc 或 phi loc。

## F. Switch Predicate 语义

43. switch case literal 与 switch value 的宽度如何处理？
    - S8/S9 已规范化 case value 类型；S10 只检查类型一致，不插入 cast。

44. switch case 比较使用什么 op？
    - 使用 normalized `Eq`，结果 bool。

45. default guard 是否应包含全部 case 的 OR 后取反？
    - 是。`default = block_guard && !(case_guard_without_block_0 || ...)`，避免 default 与 case 同时生效。

46. 多个 case value 相同怎么办？
    - S10 报错或警告？推荐报错，因为 switch lowering 会产生重叠 guard，应该由前序 validate 禁止。

47. switch 没有 default target 是否允许？
    - 不允许。S9 terminator 结构应有 default target；缺失则 S10 报错。

## G. Outputs 与端口语义

48. S10 是否只输出 output/mutable-ref final binding？
    - 输出所有 port metadata，但 `final_value` 只对 output/mutable-ref 必须存在；input value 保留 `initial_value`。

49. output final value 的 guard 是否需要单独记录？
    - 记录 `final_value`，其 definition graph 自带 guard；另可记录 `final_guard` 为 exit block guard，方便 S17 覆盖验证。

50. 若 output final value 是 initial value，表示什么？
    - 表示该 output/mutable-ref 在所有可达路径上保持原值；允许。S17 可以根据端口策略决定是否需要诊断。

51. pure output 是否允许读取 initial value？
    - 当前 S9 已允许 output/mutable-ref 有 initial value。S10 不改变该语义；是否禁止 pure output read 应在 S9/S17 统一确认。

52. 多路径 output 更新是否在 S10 合成一个最终 mux？
    - 如果 S9 final value 已是 phi，S10 phi lowering 会自然合成 mux。S10 不额外扫描 output assignments。

## H. Lookup 与动态索引读

53. `Lookup` 是否应该在 S10 展开成 mux chain？
    - 暂不展开。`Lookup` 是纯数据 op，不是控制流；保留到后续 backend normalize/opt 更合适。

54. `Lookup` 的 guard 如何处理？
    - 作为该 definition 的 guard；如果 guard false，该 value 不应被有效输出依赖，后续由 guard/phi 关系保证。

55. `Lookup` elements 是否允许来自不同 guard 定义的 values？
    - 允许。SSA/predicate value graph 表示数据依赖，guard 覆盖由 S17 验证。

## I. 类型、signedness 与字面量

56. S10 是否改变整数 op 语义？
    - 不改变。S8 已完成 operation normalization；S10 只搬运 S9 op/literal/signed_view。

57. signed_view 是否保留在每个 operand use 上？
    - 是。S10 value/signal 本身仍不携带 signedness。

58. guard op 的 signed_view 如何设置？
    - 布尔 guard operands signed_view=false；switch/branch 原 condition operand 保留其已有 operand view，但 condition type 必须 bool。

59. Mux result type 如何决定？
    - then/else incoming type 必须一致，mux result 为该 type；condition 必须 bool。

60. Literal 是否继续使用 `vector<uint64_t> + valid_width + is_signed`？
    - 是，直接复用或拷贝 S9/S8 literal 结构。

## J. 验证与错误处理

61. 错误报告使用哪套机制？
    - 使用 `RTLZZException` / `ErrorContext`，stage 填 `s10predicate`，note 包含 block/value/phi/edge 信息。

62. `verifyPredicateProgram` 应检查哪些不变量？
    - dense value ids；每个 non-initial value 有唯一 definition；definition guard 为 bool 或 true literal；operand value 存在；type 一致；无 CFG/phi/terminator；output final value 存在且类型匹配 port symbol。

63. 是否检查 guard 覆盖完整？
    - S10 只做结构性检查，不做完整覆盖证明。复杂覆盖、多驱动、未覆盖输出留给 S17。

64. 是否检查 value def dominates/use？
    - CFG 已被消除，不再使用 dominance；检查所有 operand 引用已有或允许前向引用的 value，并确保 value graph 无非法循环。推荐第一版允许任意顺序引用，但 verify 检查无 value dependency cycle。

65. 遇到 S9 中残留 unreachable but referenced value 怎么办？
    - 若 reachable output/definition 依赖 unreachable block definition，报错；正常 S9 不应产生这种引用。

66. 遇到 S9 中 residual `Unreachable` terminator 的 reachable block 怎么办？
    - 可接受，表示该路径不通向 output；block statements 可以按其 block guard 输出，但没有 successors。若它影响 final output 缺失，应由 S9/S17 捕获。

## K. Guard 表达式形式与优化边界

67. guard `And/Or/Not` 应使用 S8/S9 op kind 还是新增 predicate op kind？
    - 复用 normalized op：`BoolAnd`、`BoolOr`、`LogicalNot`。为减少枚举扩展，推荐复用 S8 op kind。

68. phi mux 是否使用 `Mux` op kind？
    - 是，复用 `S8OpKind::Mux`，condition bool，then/else 同 type。

69. 是否在 S10 中做 CSE，共享相同 guard op？
    - 先不做全局 CSE，保持实现直接；后端优化再合并。

70. 是否消除 true guard 下的 guard field？
    - 结构中仍可显式保存 true literal guard；debug print 可显示 `guard=true`，避免空值语义歧义。

71. 是否生成 block guard value 表？
    - 是。输出/summary/debug 中可记录每个 reachable block 的 guard value/literal，便于验证 phi lowering。

72. edge guard 是否需要保留在输出中？
    - 可作为 debug metadata 保留，不作为核心语义；phi lowering 后数据依赖已显式化。

## L. Debug Print 与测试需求

73. debug print 应显示哪些内容？
    - summary、symbols/ports、values、block guards、definitions、output final bindings；phi lowering 应标注来源，如 `lowered_phi`.

74. 单元测试应覆盖哪些手写 S9 场景？
    - 直线 block true guard；if/else block guard；phi to mux；nested branch guard；switch case/default guard；unreachable block ignored；lookup 保留；output final binding。

75. 集成测试应覆盖哪些 cpp 到 S10 场景？
    - 简单 if/else 输出、nested branch、switch、多路径 output、动态数组读写经 S9 后到 S10、循环展开后的 branch/continue/break 场景。

76. 是否使用 golden debug string？
    - 使用关键片段断言，不写整文件 golden，避免 generated guard value id 变化导致测试脆弱。

77. 是否应测试 no-CFG/no-phi/no-terminator 不变量？
    - 是。S10 test 应检查 debug 或 verify 保证输出中不再存在 block terminator/phi 语义。

78. 是否加入 CMake 自动目标？
    - 当前 `testv2/*.cpp` 已自动发现；新增 `testv2/s10predicate_test.cpp` 即可。

81. S10 输出是否作为 PredicateVerifyAndSimplify 的唯一输入？
    - 是。后续不应再处理 CFG/phi/branch/switch，只面对 flat predicate assign list 与 output bindings。

82. 是否需要现在修改 roadmap 中 S17 编号？
    - 暂不需要。本阶段只准备 S10 checklist；若后续阶段编号从 10 跳到 17 是历史规划残留，可在开始 S17 前统一整理。
