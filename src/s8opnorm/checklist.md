# S8 OperationNormalize Checklist

S8 输入为 S7 的 `s7flatten::S7FlattenedProgram`，其中已经没有 aggregate、struct、array lvalue、call、return slot 或复杂表达式树。S8 的职责应限定为：把 S7 中仍然较高层的整数/bitvector operation 规范化为宽度、符号、字面常量和 operand shape 全部明确的 normalized scalar operation，为后续 SSA / predicate lowering / backend IR 提供稳定基线。

以下问题需要在编码前确认。每项给出推荐语义；若无异议，后续实现会按推荐语义落地。

## A. 阶段边界与输出结构

1. S8 输出是否应定义新的 `S8NormProgram/S8NormCFG/S8NormStmt/S8Value`，而不是复用或原地修改 S7 结构？
是。S8 应作为新的阶段契约，输入 S7，输出 S8；后续阶段不应继续理解 S7 的高层 op 枚举。

2. S8 输出是否仍保持 CFG 形态，不做 SSA、不做控制流谓词化？
是。S8 只规范化 basic block 内的 operation 和 terminator operands，不插入 phi，不改 CFG 拓扑。

3. S8 是否允许新增临时 symbol？
允许，但仅当复杂 op 需要拆成多条 normalized op 时新增；新增 symbol id 必须保持 function 内唯一、dense 或可验证引用有效。

4. S8 是否继续保留 debug name？
保留在 symbol/debug metadata 中，仅用于 debug print；语义引用仍只使用 symbol id。

5. S8 是否继续保留 `S7SymbolRole::{Local,Port,Temp}` 等角色？
保留等价的 `S8SymbolRole`，便于端口和临时变量可读，但后续语义不依赖 debug name。

6. S8 是否继续保留 `Lookup/LookupWrite`？
保留，但将 index/value/elements 全部规范化为 normalized operands，index 的整数语义也要明确。`Lookup/LookupWrite` 是动态数组 flatten 的结果，不属于整数 op normalization 要消除的对象。

7. S8 是否处理 CFG terminator 的 branch/switch condition/value？
处理。branch condition 必须规范化为 1-bit bool；switch value 和 case literal 必须规范化成相同 width/signedness 的 normalized value。

8. S8 是否允许 S7 中的任何 type width <= 0 继续进入输出？
不允许。所有 symbol、literal、operation result 都必须有确定 positive bit width。

9. S8 是否允许输出中继续出现 `TypeInfo`？
保留一个收窄后的 `NormType { kind, width }`，不要继续暴露完整 `TypeInfo`，避免 struct/array/reference 等无关字段重新进入后续阶段。

10. S8 是否应提供 verifier？
是。实现 `verifyNormProgram()`，检查所有 symbol id、operand width、operation arity、result width、literal width、terminator condition width。

## B. 统一整数类型模型

11. S8 是否把所有 scalar 类型统一成 bitvector integer 类型？
是。统一为 `NormIntType { int width; bool is_bool; }`，其中 bool 是 width=1 的无符号/逻辑类型。注意：信号不包含 signed 信息，signed信息位于OPrand中，只有当一个信号作为操作数时才会影响该OP是否以符号视图获取信号值。

12. `bool` 是否独立于 `UInt<1>`？
语义上区分，但norm允许其互相转换。

13. `Int<N>` 的 signedness 如何定义？
信号不包含 signed 信息，signed信息位于OPrand中，只有当一个信号作为操作数时才会影响该OP是否以符号视图获取信号值。

14. C++ builtin `int/unsigned/uint*_t/int*_t/bool` 如何映射？
映射到确定宽度 bitvector：`int`=signed 32，`unsigned int`=unsigned 32，`int8_t/uint8_t/...` 按名字宽度，`bool`=bool width 1。若 type 已给出 width，以 type.width 为准，但 signedness 必须一致或报错。

15. builtin `char/short/long/long long/size_t` 是否支持？
除非 AST 已能稳定给出 width/signedness；遇到无法识别的 builtin integer 直接报错。

16. 所有算术是否采用固定宽度 two's-complement wraparound？
否，算术计算宽度信息以 fixint.hpp API 为准：加法返回 (max(lhs.width, rhs.width) + 1)，乘法返回 (lhs.width + rhs.width)，其他算术操作都按 fixint.hpp 规则 wraparound。

17. signedness 是否影响存储位模式？
不影响位存储，只影响 signed compare、arithmetic right shift、sign extension、literal interpretation、cast，以及常量 RHS 的 Div/Mod lowering。

18. signedness 是否影响 `Add/Sub/Mul` 的位级结果？
必要时影响。

19. signedness 是否影响 `Div/Mod`？
仅允许第二操作数为常量的特殊 lowering。unsigned operand view 支持：2 的幂常量转换为移位/截取；非 2 的幂常量使用 magic multiplier / correction 路径转换为乘法、加减法、移位和截取。signed operand view 先计算 quotient/remainder 的输出符号和绝对值舍入方向，再对 dividend/divisor 取绝对值并复用 unsigned 常量 lowering，最后恢复结果符号；非常量 RHS 仍报错。

20. S8 是否允许 width 为 0 的 bool 或 unknown integer？
不允许，S8 前必须有完整类型信息。

## C. 字面常量解析

21. normalized literal 内部表示是否采用 `vector<uint64_t> words + valid_width + is_signed`？
是。words 使用 little-endian word order，即 `words[0]` 保存最低 64 bit。

22. literal 是否还保留原始字符串？
可作为 debug metadata 保留，但语义使用 parsed words/width/signed。

23. literal 的 `valid_width` 来源是什么？
按照C++风格的字面量后缀解析。

24. 十进制 literal 没有显式类型时如何处理？
必须依赖 S7 operand type 或目标上下文给出 width/signed；S8 不做 C++ literal type inference。

25. 支持哪些 literal 文本格式？
支持 `true/false`、十进制、十六进制 `0x`、二进制 `0b`、可选前导 `+/-`。支持单引号分隔符如 `1'000` 可作为可选项；推荐支持并忽略分隔符。

26. 是否支持 C++ 后缀如 `u`, `U`, `l`, `LL`？
支持。

27. 负数字面量如何表示？
按目标 width 解析补码。

28. 负数字面量如果目标类型 unsigned 是否允许？
允许。

29. 字面量超过目标 width 怎么办？
报错。

30. 字面量解析是否做 constant folding？
先只解析和规整。

31. bool literal 非 `0/1/true/false` 是否允许？
不允许直接作为 bool literal；整数到 bool 必须通过显式 cast/compare normalization。

32. literal words 是否始终 mask 到 valid_width？
是。最高 word 的无效高位必须清零。

## D. 赋值与 cast 规则

33. `Assign dst = value` 中 value width 与 dst width 不一致如何处理？
S8 显式插入/生成 normalized extend/trunc op，最终 assign 两侧 width 必须一致。

34. assignment 的隐式转换采用什么规则？
按目标类型转换：目标更宽则 signed source 用 sign-extend、unsigned/bool source 用 zero-extend；目标更窄则 truncate。

35. `Cast` 是否被消除为 `SExt/ZExt/Trunc/Bitcast`？
是。S8 输出不再保留高层 `Cast`；根据 source/target width/signedness 生成明确 normalized op。等宽仅 signedness 改变时生成 type-only bitcast 或直接更新 result type，需确认。

36. 等宽 signed/unsigned cast 是否需要 op？
N/A，因为输出的信号已经不包含signed语义。

37. bool cast 到整数如何处理？
bool -> integer 使用 zero-extend 到目标 width。

38. integer cast 到 bool 如何处理？
生成 `ReduceOr`，结果 bool width=1，而不是简单取 bit0。

39. cast 到相同 width/signedness 是否消除？
消除。

40. S8 是否允许隐式改变 signedness 但不改变 width 的 assignment？
N/A，因为输出的信号已经不包含signed语义。

## E. 算术、比较、逻辑与位运算

41. binary op 两个 operand width 不一致时如何处理？
提升到更宽操作数，乘法允许不同宽输入。

42. 共同 width 如何选择？
提升到更宽操作数，乘法允许不同宽输入。

43. 算术 op 结果 width 是目标 width 还是推导 width？
以 fixint.hpp API 为准推导width，再插入转换成目标 width。

44. `Add/Sub/Mul` 是否允许 signed/unsigned 混合？
允许。

45. `Div/Mod` 的 result signedness 如何决定？
结果信号仍不携带 signedness。unsigned 常量除数直接 lowering；signed 常量 Div 的输出符号为 `lhs_negative xor rhs_negative`，signed 常量 Mod 的输出符号为 `lhs_negative`，两者均按绝对值执行常量除法/取模 lowering 后恢复符号。

46. `Shl/Shr` 的 RHS shift amount width 如何处理？
不处理，允许shift amount operand width不一致。

47. `Shr` 对 signed LHS 是否表示 arithmetic shift？
是，内含在 SHR OP 的 Oprand 的signed位中。

48. `LogicalAnd/LogicalOr` 是否保留为 binary op？
先把 operands cast-to-bool 再生成 binary bool op。

49. unary `LogicalNot` 如何处理？
operand cast-to-bool，然后生成 unary bool not。

50. unary `BitNot` 的 result width 如何处理？
先同宽度计算，然后 cast 到 target width。

51. unary `Negate` 如何处理？
先同宽度计算，然后 cast 到 target width。

52. unary `Plus` 如何处理？
先同宽度计算，然后 cast 到 target width。

53. 比较 op 输出类型是否固定 bool width=1？
是。`Eq/Ne/Lt/Le/Gt/Ge` 结果必须为 bool width=1。

54. signed 比较如何决定？
N/A，同上。

55. equality 比较是否关心 signedness？
位模式比较，signedness 不影响 `Eq/Ne`。

## F. Ternary / mux

56. `S7OpKind::Ternary` 是否规范化为 explicit mux/select op？
是。S8 输出使用 `Mux(cond, then, else)`，不再叫 Ternary。

57. ternary condition 如何处理？
condition cast-to-bool。

58. then/else width 不一致如何处理？
都Cast到 target width。

59. then/else signedness 不一致如何处理？
N/A，因为输出的信号已经不包含signed语义。

60. Mux 是否允许 bool result？
允许，then/else 都规范化为 bool width=1。

## G. Hardware operations

61. `ZExt/SExt/Trunc` 在 S8 后是否继续存在？
：继续存在，但作为 normalized explicit op，必须满足 source/target width 关系。

62. `ZExt` 目标宽度小于等于 source width 怎么办？
若等宽 no-op；若更小报错，应该使用 `Trunc`。

63. `SExt` 目标宽度小于等于 source width 怎么办？
若等宽 no-op/bitcast；若更小报错，应该使用 `Trunc`。

64. `Trunc` 目标宽度大于等于 source width 怎么办？
若等宽 no-op；若更大报错，应该使用 extend。

65. `Slice(base, hi, lo)` 输出 width 如何定义？
先输出`hi - lo + 1`，再 cast 到 target width。

66. `BitSelect(base, bit)` 输出类型如何定义？
bool width=1，`0 <= bit < base.width`。

67. `DynamicSlice(base, index)` 的 output width 如何确定？
DynamicSlice 操作本身指定了目标位宽。动态宽度slice是不支持的。

68. `DynamicBitSelect(base, index)` 输出类型如何定义？
bool width=1。

69. `WriteSlice(base, hi, lo, value)` 输出 width 如何定义？
输出与 base width 相同；value 必须 cast 到 `hi-lo+1` width；slice bounds 必须合法。

70. `WriteBit(base, bit, value)` 输出 width 如何定义？
输出与 base width 相同；value cast-to-bool；bit bounds 必须合法。

71. `DynamicWriteSlice(base, index, value)` 输出 width 如何定义？
输出与 base width 相同；value width 决定动态 slice width，或由 operation metadata/target type 决定。推荐使用 value normalized width，要求 `value.width <= base.width`。

72. `DynamicWriteBit(base, index, value)` 输出 width 如何定义？
输出与 base width 相同；value cast-to-bool。

73. dynamic bit/slice index 的 signedness 如何处理？
N/A.

74. `Concat(parts...)` 的 output width 如何定义？
sum(parts.width)，cast to target width；part order 按 S7 operands 顺序从高位到低位。

75. `Repeat(value, times)` 的 output width 如何定义？
`value.width * times`，times 必须 > 0 ，cast to target width。

76. `ReduceOr/ReduceAnd/ReduceXor` 输出类型如何定义？
bool width=1。

77. reduce operand width 为 0 或 bool 是否允许？
width 必须 > 0；bool width=1 允许。

78. `Concat/Repeat` 的 signedness 如何定义？
N/A

79. Hardware op operand 个数是否严格校验？
是。每种 op 固定 arity；`Concat` 可变 arity但至少 1；`Repeat` arity 1 且 times > 0。

## H. Lookup / LookupWrite

80. `Lookup(index, elements...)` 的 elements width/signedness 是否必须一致？
是。若不一致，按 target type 对每个 element 做转换后再 lookup。

81. `Lookup` index literal 越界如何处理？
index 不应为 literal，无需处理（literal index已经在S7被完全展开）。

82. `Lookup` target width 是否决定 element cast？
是。elements 全部 cast 到 target type。

83. `LookupWrite` 的 target symbols 是否必须同类型？
否，可以cast即可。

84. `LookupWrite` value 如何转换？
根据自身signed提升到target symbols最宽值，再cast到目标type。

85. `LookupWrite` index 处理规则是否与 `Lookup` 一致？
一致，不处理。

86. `LookupWrite` 是否在 S8 展开为 mux/guarded assign？
否。S8 只规范化类型和字面量；动态写 lowering 到 guarded write 应留给后续 predicate/control lowering。

## I. Switch / branch / condition

87. Branch condition 是否允许非 bool integer？
允许输入，但 S8 输出必须插入/表示 cast-to-bool。

88. Switch value 和 case value 的类型如何统一？
case literal cast 到 switch type。

89. Switch case literal 是否需要解析成 normalized literal？
是。

90. Switch case value 超出 switch width 怎么办？
严格报错，避免 case 被静默截断导致重复或错误匹配。

91. 是否检查重复 case？
是，在 normalized literal bit pattern 上检查重复。

## J. 错误处理与严格性

92. S8 对无法规范化的类型/操作应报错还是保留原样？
报错。OperationNormalize 是后续语义基线，不应把未知语义传给 SSA/backend。

93. 是否允许 warning 后继续？
仅对非语义 debug 情况允许 warning；任何 width/signedness/literal/op 语义不确定都应 error。

94. 是否提供 strict/lax options？
暂时只提供 strict 默认语义；后续如需兼容可添加 option。

95. 错误信息是否应包含 source loc、stage、symbol/op 信息？
是。使用现有 `RTLZZException` 和 stage=`s8opnorm` 或 `s7norm`，错误文本包含 block id、stmt kind、target symbol debug name。

## K. 命名与目录

96. 目录名用户指定为 `src/s8orm`，但 roadmap 中这是第 8 阶段。命名是否使用 namespace `pred::s8norm` 还是 `pred::s8opnorm`？
改为 `src/s8opnorm`。

97. 输出类型命名是否使用 `S7NormProgram` 还是 `S8NormProgram`？
`S8NormProgram`。

98. public API 名称是否为 `normalizeOperations()`？
是。提供 `normalizeOperations(const s7flatten::S7FlattenedProgram&, const NormOptions&)` 和 `normalizeOperationsOrThrow()`。

99. debug print 是否需要作为测试主入口？
是。和前序阶段一致，提供 `debugPrint(const S8NormProgram&, summaries)`，测试先以 debug print 和结构 verifier 双重验证。

100. 是否需要在本阶段加入大量单元测试和 S1->S8 集成测试？
是。先覆盖 literal parsing、cast/extend/trunc、arithmetic/compare/logical、hardware ops、lookup/lookupwrite、branch/switch condition，再补 source-to-S8 集成用例。
