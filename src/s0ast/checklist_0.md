# S0.0 ClangSessionAndRawCursor Checklist

目标：建立 V2 专用 libclang parse session，收集源文件归属、diagnostics、top/helper/lambda 相关 raw cursor 信息；此阶段不构造最终语义 AST，不做 API lowering。

## 需要确认的语义点

1. C++ 标准版本默认值是什么？
   - V2 默认 `-std=c++20`，同时允许 pipeline config 追加 clang args 覆盖或补充。

2. 源文件没有显式包含 `fixint.hpp` 时是否仍自动注入？
   - 保留自动注入，但只在 V2 parse session 内处理，不写临时 wrapper 到项目目录；source loc 仍指向用户源文件。

3. `source_text` 内存输入是否必须和文件输入完全一致地支持？
   - 必须支持，测试和脚本会依赖内存输入；`#line` 应保证 diagnostic loc 指向原 source name。

4. top function 查找是否继续支持 wildcard？
   - 保留旧行为：允许 wildcard，但匹配多个 function 时报错。

5. helper function 的收集范围是什么？
   - 仅收集与 top 同一用户源文件中的非 top function definitions；不收集 `third_party/vulsim/vullib` 内函数。

6. 是否收集 forward declaration？
   - 收集 declaration metadata 用于 overload/name diagnostics，但只有 definition 可作为 helper/lambda inline callee。

7. 是否允许跨文件 helper？
   - 不允许。

8. diagnostics 策略是什么？
   - clang error 直接失败；warning 默认记录但不失败；V2 可以提供 option 将 warning 升级为失败。

9. raw cursor 是否在阶段结果中长期保存？
   - 只在 S0 内部临时使用；输出 V2AST 不持有 `CXCursor/CXType/CXTranslationUnit`，避免生命周期风险。

10. 是否继续做早期 `checkSubset`？
    - 拆到 S0.4，S0.0 只做 clang parse diagnostic 和 source ownership 过滤。

11. libclang parse option 是否需要 `CXTranslationUnit_DetailedPreprocessingRecord`？
    - 默认不开；只有需要宏/source token diagnostics 时再加 option。

12. 如何处理宏展开中的代码？
    - 第一版拒绝 top/helper/lambda body 中来自宏展开且无法稳定定位的语句；允许宏定义常量在 clang 已解析为 literal 时使用。

13. 如何识别“用户源文件”？
    - 以 normalized primary source path 为准；`source_text` 使用 synthetic source_name；wrapper/fixint/include 都不算用户源。

14. 输出 debug 信息包含什么？
    - top resolved name、source file、collected helper candidates、lambda declarations、struct declarations、clang diagnostics summary。

