# S11 BEIR Checklist

本阶段目标为：将 `s10predicate::S10PredicateProgram` 转换为现有后端 `pred::beir::Program`，使当前新 pipeline 能接入原 BEIR optimizer / RTL emitter。这里仅列 S10 结果与原 BEIR 结构差异所导致的语义歧义点；常规 op 枚举映射、debug print、测试覆盖等实现细节不在本 checklist 展开。

## 1. S10 guarded definition 到 BEIR guardless driver 的语义

**差异：**
S10 的每个 `S10Definition` 带 `guard`，表示该 value 只在某个 predicate 下语义有效；BEIR 的 `Signal::driver` 没有 guard 字段，每个 signal 只有一个无条件 driver。

**推荐方案：**
在 S11 中不要把每个 guarded definition 包成 `Ite(guard, value, undef/default)`；而是依赖 S10 内部 readonly check 已证明“每个 value 只在其 definition guard 覆盖的上下文中被读取”。因此转换到 BEIR 时可将每个 S10 value 的 definition 作为无条件 driver 发出，guard 仅作为 debug metadata 或诊断信息保留。

**需要确认：**
是否接受“guard 在 BEIR value driver 中被擦除，仅由 S10 readonly check 保证条件化读取合法”的语义？如果不接受，则 S11 必须为每个 guarded value 合成 default/hold 值，这会重新引入默认值策略，且可能改变当前 S10 的精确语义。

Yes

## 2. output final binding 是否需要额外 ITE/default

**差异：**
S10 的输出由 `S10Port::final_value` 指向某个 S10 value，且有可选 `final_guard`；BEIR output 是一个 named output signal，必须有一个无条件 driver。

**推荐方案：**
要求 `final_guard` 必须等价于 true，或至少由 S10 readonly check 证明 output final value 在 true 下可用；S11 直接令输出 port signal driver = `Assign(final_value_operand)`。不要在 S11 为输出再合成默认值或 guarded assignment merge。

**需要确认：**
是否规定 S10 之后所有 output/mutable-ref final binding 在 BEIR 入口必须是 total value？若允许 `final_guard != true`，需要确认 false 路径输出是保持 initial、置零、还是报错。

Yes，不完全输出路径应报错。

## 3. S10 value id 与 BEIR Signal 命名/可观察性

**差异：**
S10 使用 dense `S10ValueId`，同一个 base symbol 有多个 SSA version；BEIR 使用 named `Signal`，port observability 依赖 `port_name`、`program.inputs/outputs` 和 signal name。

**推荐方案：**
为每个 S10 value 创建一个 BEIR signal，名字使用稳定且唯一的 debug 名：
`<base_debug_name>_v<version>`；generated value 使用其 `debug_name` 加 value id。output port 的实际 BEIR output signal 使用原 port/base symbol 名称，并由 `Assign(final_value)` 驱动。SSA version signals 作为内部 signals。

**需要确认：**
BEIR output 名是否必须保持原端口名，而不是 `out_vN`？推荐保持原端口名，否则 RTL 端口名会改变。

是，保持。

## 4. input / mutable-ref initial value 的 BEIR 表示

**差异：**
S10 initial values 是普通 value id；BEIR 对 input port 通过 `PortRead` driver 连接端口。对 output/mutable-ref 的 initial value，旧 BEIR 没有明确 inout/ref 端口模型，且目前后端拒绝 `InOut`。

**推荐方案：**
S11 将 `ParamDirection::Input` 的 initial value 映射为 `PortRead`。对于 `Output + MutableRef` 且 S10 中存在 initial value，若该 initial value 被实际依赖，需要把这个端口视作“可读输出”：推荐在 BEIR 中仍创建同名 output port，同时为其 initial SSA value 创建一个 internal signal，以 `PortRead` 从同名 port 读取旧值。但这可能与现有 RTL output port 方向冲突。

**需要确认：**
mutable-ref/output 的 initial value 在后端应如何表示？

推荐二选一：
1. 短期硬件子集禁止读取 output/mutable-ref 初始值，S11 若发现 output initial value 被 final graph 依赖则报错。
2. 支持 read-write ref port，但需要扩展 BEIR/RTL port direction 或引入独立 input shadow port。

禁止读取输出port的初始值，发现应直接报错。

## 5. S10 `Lookup` 与 BEIR `Lookup` 输入形式不一致

**差异：**
S10 `Lookup` 是 `lookup(index, elem0, elem1, ...)`，元素已经是 scalar operands；现有 BEIR `OperationKind::Lookup` 的旧路径从 `Call("lookup")` 来，`validateOperationTypes()` 当前要求第一个 operand 是 array base operand，而不是 element list。

**推荐方案：**
不要直接映射到现有 BEIR `Lookup`，除非同步调整 BEIR `Lookup` 语义。更稳妥的 S11 方案是把 S10 lookup lowering 成 mux tree：
`Mux(index == 0, elem0, Mux(index == 1, elem1, ...))`，全部使用 BEIR scalar ops。

**需要确认：**
S11 是否应在转换时消除 S10 `Lookup`，而不是保留 BEIR `Lookup`？推荐：消除成 mux tree，以避免修改旧 BEIR `Lookup` array-base 语义。

保留BEIR Lookup。将S10 Lookup中的element list先构造为 BEIR array 的临时信号（通过 BEIR array 声明与赋值方式），再使用 BEIR Lookup(array, index) 进行查找。

## 6. S10 normalized op 到 BEIR OperationKind/OpCode 的精确映射

**差异：**
S10 使用 S8/S9 normalized op kind，例如 `BoolAnd/BoolOr/LogicalNot/Mux/LShr/AShr/DynamicSlice/DynamicWrite*`；BEIR 使用 `OperationKind::{Binary,Unary,Ite,...}` + `OpCode`，右移 signedness 依赖 operand `signed_view`，没有独立 `AShr` OperationKind。

**推荐方案：**
按 BEIR 现有表达能力映射：
`Mux -> Ite`；`BoolAnd/BoolOr -> Binary LogicAnd/LogicOr`；`LogicalNot -> Unary LogicNot`；`LShr/AShr -> Binary Shr`，其中 AShr 的 lhs operand 设置 `signed_view=true`；其余 slice/ext/reduce/concat/repeat/dynamic bit/slice/write 按同名 BEIR operation 映射。

**需要确认：**
`AShr` 是否必须通过 lhs operand `signed_view=true` 表达，而不是新增 BEIR op？推荐复用现有 BEIR signed_view 机制。

复用现有 BEIR signed_view 机制。不新增。

## 7. S10 literal 已是 parsed limbs，BEIR 旧路径从字符串 parse

**差异：**
S10 literal 已经是 `vector<uint64_t> + valid_width + is_signed`；BEIR 旧 builder 的 literal 入口主要从 `Expr::literal_value` 字符串解析。

**推荐方案：**
S11 直接构造 `beir::Operand::Constant`，不再 stringify/reparse。`Operand::type.width = valid_width`，`signed_view` 使用 S10 operand 的 `signed_view`，constant 的 `signed_view` 使用 literal metadata 或 operand view 的合并策略。

**需要确认：**
BEIR literal `constant.signed_view` 应取 S10 literal `is_signed`、S10 operand `signed_view`，还是二者 OR？推荐使用 `literal.is_signed || operand.signed_view`，同时 `operand.signed_view = operand.signed_view`，保持“存储不带符号、use 带符号视图”的语义。

取 S10 literal `is_signed`，同时设置使用了该 literal 作为 operand 的 operand.signed_view。

## 8. Debug/source location 映射粒度

**差异：**
S10 definition 有 `debug_loc`、`debug_note`、source block；BEIR 有 `Operation::source_locs` 和 `DebugInfo`，但没有 block/guard 字段。

**推荐方案：**
S11 把 S10 definition loc 写入 BEIR operation source_locs；guard/debug_note/source_block 写入 `DebugInfo::reason` 或 `derived_names`，仅用于诊断，不参与语义。

**需要确认：**
是否允许 S11 将 guard 信息只作为 debug reason 保留？推荐允许，语义由 S10 readonly check 保证。

不保留 guard， 只保留 debug reason 和 debug loc。

## 9. BEIR optimizer 是否会错误优化掉“条件化有效”的内部 signals

**差异：**
S10 中某些 value 只在 guard 下有效；S11 若把其 driver 无条件化，BEIR optimizer 可能 CSE/DCE/常量传播这些内部 signals。

**推荐方案：**
允许 BEIR optimizer 正常处理。因为 S10 readonly check 已要求所有依赖在有效 guard 下读取，无条件化 driver 不改变可观察输出。若某些 op 在无效 guard 下引用任意值，只要这些值本身也有 total BEIR driver，就不会产生未驱动硬件。

**需要确认：**
是否接受“无效路径上的内部 signal 值可以任意但必须有 driver”的硬件语义？推荐接受，这是 predicate lowering 后常见的组合电路语义。

接受。

## 10. BEIR Program 是否仍需要旧 `PredicateProgram` 输出表达式/lookup_table metadata

**差异：**
旧 BEIR builder 依赖 `PredicateProgram::outputs/output_expressions/lookup_tables/param_directions/symbols`；S10 不再有这些字符串 metadata，而是有 base_symbols/ports/final_value/definitions。

**推荐方案：**
S11 不走旧 `buildProgram(const PredicateProgram&)` 适配层，而是直接构造 `beir::Program`。只保留后端必需字段：`function_name`、`inputs`、`outputs`、`ports`、`signals`。`lookup_tables/output_expressions` 不再使用。

**需要确认：**
是否接受 S11 直接构造 BEIR Program，而不是先反生成旧 `PredicateProgram`？推荐直接构造，避免退回字符串 Expr 语义。

接受，直接构造 BEIR Program。
