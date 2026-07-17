# S0.2 TypeAndEntityCollect Checklist

目标：收集并 canonicalize 类型、函数、helper、lambda、struct、global const metadata，形成后续构建表达式和解析调用所需的实体表。

## 需要确认的语义点

1. 支持的 scalar 类型集合是什么？
   - `bool`、C++ fixed-width/builtin integer、`Int<N>`；`SignedView` 只作为 expression view，不作为 storage declaration type。

2. top 参数允许哪些形式？
   - 仅 `bool`/builtin integer/`Int<N>` 及其 `std::array` 静态数组；input 可 value/const-ref，output 必须 mutable-ref；不允许 struct/aggregate/pointer。

3. helper/lambda 参数是否允许 struct/array？
   - 允许 helper/lambda 内部使用 struct/array by value/const-ref/mutable-ref，前提是字段/元素不含 pointer/reference；S7 负责 flatten。

4. 是否允许 pointer 类型？
   - 全部拒绝，包括参数、local、field、array element。

5. 是否允许 reference field？
   - 全部拒绝。顶层/helper/lambda 参数 reference 只表示 passing kind，不是 storage field。

6. 是否允许 enum？
   - 转换为 underlying integer storage type，同时记录 enum debug name；不支持 scoped enum 成员作为独立语义。

7. `std::array` 支持范围？
   - 只支持静态 extent，允许多维嵌套；不支持 C array decay/pointer；不支持 raw C array。

8. raw C array 是否支持？
   - 仅支持 `std::array` 和 clang constant array local/global；top port 只支持 `std::array`。

9. struct 支持范围？
   - 只支持 standard-layout-like aggregate，字段为支持类型；不支持 virtual/base class/private access/bitfield/reference/pointer field。

10. constructor metadata 是否需要完整保留？
    - 只保留可映射到字段赋值的 constructor；复杂 constructor body 拒绝。

11. global const/static 如何处理？
    - 仅支持 integral scalar constexpr/static const 和静态数组常量；作为 read-only decl 注入到 top/helper 可见环境或单独 global const table。

12. helper overload key 如何建立？
    - 按 source name 分组，候选包含 FunctionId、参数数量、canonical param storage types、passing kind、return type。

13. lambda 是否参与 overload？
    - lambda variable name 在其 lexical scope 中是普通 callable entity；同名 helper 与 lambda 同时存在时，按 C++ lexical lookup，local lambda 优先。

14. capture 列表如何收集？
    - 显式 captures 收集到 capture table；capture-default `[&]`/`[=]` 应解析实际 odr-use captures，无法稳定解析则拒绝并给诊断。

15. 间接递归在哪个阶段检查？
    - 推荐：S0.2 可建立 entity graph，S2 做最终递归拒绝；S6 仍保留防御性检查。

