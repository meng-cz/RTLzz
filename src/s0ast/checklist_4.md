# S0.4 ResolveAndSurfaceValidate Checklist

目标：在 V2AST surface 层解析名字/作用域/重载/lambda call identity，并拒绝 V2 不支持的 C++ 子集；不做 inline、flatten、CFG 或 API lowering。

## 需要确认的语义点

1. 名字解析是否在 S0 完成？
   - 完成 lexical name resolution，VarRef 绑定 DeclId，Call 绑定 callee candidate/resolved FunctionId；S2 可复核但不重新猜。

2. overload resolution 支持到什么程度？
   - 第一版支持 exact canonical type match、array/reference passing kind match、const-ref/value 基本匹配；不实现完整 C++ conversion ranking。

3. helper overload 二义性如何处理？
   - 只要 exact match 不唯一就报错，给出候选签名。

4. lambda 与 helper 同名时如何处理？
   - 遵循 lexical lookup，局部 lambda variable 优先于 global/helper function；同 scope 重名时报错。

5. captures 如何参与名字解析？
   - lambda body 内 capture 绑定到 captured DeclId；后续可作为 hidden param 或 environment binding，不允许解析成 `auto` lambda object storage。

6. 是否允许 capture-default？
   - 允许但必须能解析实际 captures；无法解析实际 captures 时拒绝，并建议用户显式 capture。

7. 是否允许 lambda 捕获另一个 lambda？
   - 允许。

8. 是否允许 nested lambda？
   - 允许一层或多层 nested lambda，但必须有稳定 FunctionId 和 capture binding。

9. 递归检查范围？
   - 暂不检查，调用信息合法即可，后续再检查。

10. 不支持的 C++ 特性清单？
    - 拒绝 floating point、dynamic allocation、exceptions、virtual、function pointer/std::function、unsupported STL containers、pointer arithmetic、goto、coroutine、volatile atomic、thread_local。

11. switch 支持策略？
    - 允许switch。

12. struct/array 内 pointer/reference 如何处理？
    - S0.4 直接拒绝，错误定位到字段/element type。

13. RegProxy/ReqHelper/Queue/BRAM 旧模式如何处理？
    - 不支持，其struct成员包含引用，应当因此被直接拒绝。

14. builtin/fixint API 是否在 S0.4 校验？
    - 只校验 call target 可识别且参数数量/template args 形状合理；实际 lowering 留给 S1APINorm。

15. signed_view 合法位置是否在 S0.4 检查？
    - S0 标记 `.sint()` view；S8 做最终“仅乘法/右移lhs/数值比较/SExt”校验。S0.4 可拒绝明显把 signed_view 当 lvalue/storage 的情况。

16. S0 是否替代现有 S2？
    - 不完全替代。S0 做 surface resolve，仅报错无法被正确解析容纳的内容，S2Validate 仍保留 V2AST/S1NormedAST 后的 semantic subset 验证；职责需要在实现前明确。

