# S7Flatten semantic checklist

S7Flatten 计划输入 S6Inline 之后的 `InlinedCFGProgram`，输出仍保持 CFG 形态，但 CFG 内的变量、左值和表达式应降到 scalar leaf 级别。本阶段目标是移除 aggregate 语义和复杂左值语义，为后续 OperationNormalize / SSA / PredicateLowering 准备只含标量 leaf assignment 的程序。

请逐项确认以下语义。

## 1. 阶段输入与输出

1. S7 的输入是否固定为 `s6inline::InlinedCFGProgram`，即假定所有 helper/lambda call 已经被内联，CFG 中不应再存在普通 `Call`？
Yes

2. S7 输出是否应定义新的 `s7flatten::FlattenedProgram/FlattenedFunction/FlattenedBasicBlock`，而不是直接原地修改 `InlinedCFGProgram`？
Yes

3. S7 输出是否应保留 S6 的 block/edge/terminator CFG 结构和 block id，只替换 block 内 statement 与 symbol table？
No，定义新的Statement数据结构，需要收敛关于 field/index access 的结构和constructor/aggregate init的结构，添加新的基础statement: res = lookup(index, ele0, ele1, ...), lookupwrite(index, value, ele0, ele1, ...) 用于表示动态索引读写

4. S7 后是否允许 CFG 中继续存在 `Construct` statement？还是所有 constructor/aggregate init 都必须 lowering 成 leaf assignment 后删除？
No，必须被lowering到 leaf assignment

5. S7 后是否允许 `Assign` 的 target 是非叶子 lvalue？预期是否为“不允许，target 必须是单一 scalar leaf symbol”？
不允许。

6. S7 后是否允许 `OperandKind::LValueRead`？预期是否为“不允许，所有读都必须是 scalar leaf var 或明确 mux/hardware op 结果”？
不允许。

## 2. Scalar leaf 定义

7. scalar leaf 是否定义为“非 struct、非 array、非 pointer/reference 的 bool 或 fixed-width integer symbol”？
仅 bool/内置int/fixed-width Int

8. struct leaf 命名建议为 `root__field__subfield`；array leaf 命名建议为 `root__idx_0__idx_1`。是否接受这种稳定命名？
Yes

9. leaf symbol id 是否必须保持 function-local 唯一，并由 S7 为新 leaf 重新分配一套连续 symbol id？
Yes

10. 原 aggregate symbol 是否应从 S7 输出的 symbol table 中移除，只保留 leaf symbols？
Yes

11. 参数如果是 aggregate 类型，是否也应在 S7 中展开成多个 leaf 参数？如果允许，top-level array/struct 参数的方向、reference/output 属性如何传播到 leaf？
主函数参数不被允许是aggregate，这在之前的阶段已经被检查，仅能为bool/内置int/fixed-width Int或它们的静态宽度数组

12. 当前 S2 已禁止 top-level struct 参数，但允许 top-level static array 参数。S7 是否需要支持 top-level array port flatten？
需要，S7结果中需要定义端口（端口数组）到具体 leaf symbols 的映射关系，以便后续阶段生成端口连接和mux tree。

13. local struct/array 变量 flatten 后是否仍需要保留源变量到 leaf set 的 debug/provenance metadata？
不需要。

## 3. Struct flatten

14. struct field 顺序是否以 `struct_fields[type]` 中的字段顺序为准？
Yes

15. 嵌套 struct 是否递归 flatten，直到所有 leaf 都是 scalar？
Yes

16. struct field read `a.b.c` 是否直接替换为对应 leaf var read？
Yes

17. struct field write `a.b = x` 如果 `b` 是 aggregate，是否展开为对 `b` 下所有 leaf 的逐一赋值？
Yes

18. struct copy `dst = src` 是否展开为所有 leaf 的 pairwise assignment？
Yes

19. struct constructor / aggregate init 是否按字段顺序或 constructor metadata 的 `field_to_param` 展开？如果两者都存在，以哪一个为准？
按 constructor metadata 的 `field_to_param` 展开

20. 是否允许部分字段初始化？未初始化字段应报错、默认 0，还是保留未初始化状态给后续 Verify？
保留Unknown状态给后续Verify

## 4. Array flatten

21. S7 是否只支持编译期已知固定维度数组，遇到未知大小或动态大小直接报错？
Yes，动态数组直接报错

22. 多维数组是否递归 flatten 为所有元素 leaf，索引顺序是否按 row-major，即 `a[i][j]` 映射为 dims `[i][j]`？
Yes

23. 静态索引读 `a[3]` 是否直接替换为对应 leaf var read？
Yes

24. 静态索引写 `a[3] = v` 是否直接替换为对应 leaf assignment？
Yes

25. array copy `dst = src` 是否展开为所有元素 leaf 的 pairwise assignment？
Yes

26. array aggregate init / initializer list 当前 AST/S3 是否已有完整结构；如果没有，S7 是否只处理现有 `decl_type.init_values` 这类元数据，其他形式报错？
如果不完整应报错

27. 大数组完全 flatten 可能造成大量 leaf symbol，是否需要 S7 options 中加入最大 leaf 数量限制？
通过参数指定，暂定4096

## 5. Dynamic index read

28. 动态 array read `a[i]` 应 lowering 为 mux tree：`i==0 ? a_0 : i==1 ? a_1 : ...`，还是 lowering 为专门的 `DynamicArrayRead` op 留给后续阶段？
前面说过，使用专门的op。

29. 如果动态索引越界，语义应如何定义：报错、默认 0、保持 don't-care，还是由前端子集保证不会越界？
无法判断时保持 don't-care，后续值分析优化时处理

30. 动态索引表达式本身是否必须已经是 scalar operand；如果不是，S7 是否可以生成临时 leaf 存储索引计算结果？
索引值本身应当是简单oprand。S3已经处理。

31. 多维动态索引是否逐层 mux，还是先 flatten 成线性 index 再 mux？
线性index。

32. 动态 struct field access 不存在于 C++ 语义，是否可以直接不支持？
不支持

## 6. Dynamic index write / guarded write

33. 动态 array write `a[i] = v` 是否 lowering 为对每个 leaf 的 guarded write：
   `a_0 = (i == 0) ? v : a_0`，`a_1 = (i == 1) ? v : a_1`，依此类推？
前面说过，使用专门的 lookupwrite OP。

34. 若 CFG 中还保留控制流，S7 的 guarded write 是否只表达“动态索引 guard”，不处理控制流 predicate？
不处理控制流。

35. guarded write 在 S7 输出中应表示为 ternary assignment，还是定义专门的 `GuardedAssign` statement？
不使用guarded write。

36. 动态写 aggregate element，例如 `arr[i] = struct_value`，是否展开为每个 element 的每个 leaf guarded assignment？
展开成对每个成员的 lookupwrite op。

37. 动态写 array slice，例如 `arr[i].field = v`，guard 应只作用到被写 leaf，未涉及 leaf 不生成赋值，对吗？
是，展开成对目标成员的 lookupwrite op。

38. 对同一个 leaf 的多次动态写是否保持原 statement 顺序逐条生成，不在 S7 合并？
不合并。

39. 动态 bit/slice 写 `DynamicWriteSlice/DynamicWriteBit` 是 Int 硬件 op，不是 array flatten。是否应保留为 hardware op，直到后续 LValueLowering/OperationNormalize？
保留，不处理。

## 7. Complex lvalue lowering

40. S7 是否负责处理所有 `LValue` access 链，使 `target` 只剩 root scalar leaf，无 field/index access？
是

41. 复杂 target 中的 index operand 若带 side-effect，按 S3 语义应已 statementized。S7 是否仍需要验证 index operand 无 side-effect？
假定index无副作用即可。如果信息足够二次验证则验证。

42. `lhs = rhs` 中 lhs 是 aggregate subtree、rhs 是 scalar 是否应报错，除非类型/leaf 数量匹配？
不匹配类型赋值应报错。

43. `lhs_leaf = aggregate_rhs` 是否总是非法？
不匹配类型赋值应报错。

44. 对 `WriteSlice/WriteBit` 这类以 scalar Int 为 base 的 read-modify-write op，S7 是否只要求其 base/value operands 已是 leaf，不降低 bit/slice 写本身？
是。仅处理复杂操作数，不处理OP本身。

## 8. Operations and temporary values

45. HardwareOp `Concat/Repeat/Reduce/ZExt/SExt/Trunc/Slice/BitSelect/DynamicSlice/DynamicBitSelect` 的 operands 是否必须在 S7 后都是 scalar leaf/literal，不允许 lvalue read 或 aggregate operand？
是。仅处理复杂操作数，不处理OP本身。

46. `DynamicSlice/DynamicBitSelect` 这里是 Int 的动态 bit range read；是否继续保留为 hardware op，不转换为 mux？
是。仅处理复杂操作数，不处理OP本身。

47. Binary/Unary/Ternary/Cast op 如果输入是 aggregate 是否直接报错？
?:操作数可以被flatten，其他操作数不允许aggregate。

48. S7 生成 mux/ternary 时，是否使用现有 S3 `OpExpr::Kind::Ternary`，并用新增临时 leaf 承接中间值？
S7不生成mux/ternary，索引赋值使用lookup/lookupwrite op。

49. S7 是否允许新增临时 symbol？如果允许，命名建议 `__s7_flatten_<hint>_<n>`，是否接受？
理论上不会新增临时symbol，仅会新增leaf symbol。

## 9. CFG 与控制流交互

50. S7 是否不改变 CFG 拓扑，只在基本块内部展开 statements？
是

51. 如果一个 block 中 aggregate assignment 展开成多条 leaf assignment，这些 statements 是否保持在同一 block 内，顺序不变？
是

52. terminator condition / switch value / return value 是否也必须 flatten 为 scalar operand？
是

53. return slot 如果是 aggregate 类型，是否需要展开为多个 return leaf slot？不过 S6 后 top 应无 helper return，是否可以只验证并报错？
已经没有return slot了，S6已经处理。验证并报错。

54. S7 是否需要重新计算 predecessors/successors？若不改 CFG 拓扑，应直接拷贝。
不改变CFG拓扑，直接拷贝。

## 10. Unsupported cases and diagnostics

55. 遇到 pointer/reference 字段、动态大小数组、未知 struct metadata，S7 应报错还是假定 S2 已拦截，只做 assert-style verify？
虽然 S2 已拦截，但 S7 仍应做 verify。

56. 遇到无法 flatten 的 constructor/call/aggregate op 时，错误信息是否需要指出 symbol、type、block id 和 stage？
需要。

57. S7 是否需要 debug print 展示原 aggregate symbol 到 leaf symbols 的映射？
需要。

58. S7 是否需要输出 summary：每个 function 展开了多少 aggregate symbol、生成多少 leaf symbol、lower 多少 dynamic read/write？
是

59. 是否需要保留 source debug loc 到所有展开出的 leaf assignment？
是

60. 如果 leaf 数量或 mux 分支数超过阈值，默认应报错还是 warning？
报错。

## 11. 测试期望

61. 单元测试应覆盖：struct field read/write、nested struct、static array read/write、multi-dimensional array、aggregate copy、constructor/init、dynamic array read、dynamic array write、dynamic write in branch？

62. 集成测试应从 C++ source 走完整 `ParseAST -> S1 -> S2 -> S3 -> S4 -> S5 -> S6 -> S7`？

63. debug print 测试以 leaf symbol mapping 和 flattened statements 为主，而不验证具体临时编号的完整字符串？
