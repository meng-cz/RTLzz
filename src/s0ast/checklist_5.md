# S0.5 PipelineBridge Checklist

目标：把 S0V2 接入现有 V2 pipeline；定义 bridge、debug print、测试和迁移边界。

## 需要确认的语义点

1. S0V2 输出直接给 S1APINorm，还是先 bridge 成现有 `FunctionAST`？
   - 直接给S1APINorm。

2. 是否允许临时 bridge 到 `FunctionAST` 以降低改动量？
   - 不考虑，请完全重构。

3. S1APINorm 是否需要改成接收 S0V2AST？
   - 是。S1 应改成接收 S0V2 surface AST，并输出 `S1NormedAST`；旧 S1 接口可保留给测试。

4. pipelinev2 默认何时切到 S0V2？
   - 立即切换。

5. CLI 是否需要暴露 S0 debug format？
   - 新增 `--format s0ast` 或内部测试入口，输出稳定 debug print，方便调 lambda/operator 解析。

6. 错误报告格式？
   - 全部使用 `RTLZZException`/`ErrorContext`，stage 使用 `s0ast.N`，包含 source loc、cursor kind、callee/operator/type note。

7. 与 `portmeta` 快速路径如何集成？
   - portmeta 只运行 S0V2 + 必要 S1/S2/S7 metadata 阶段；不依赖完整 backend。

8. 单元测试最小集合？
   - 分别测 type recognition、lambda call recovery、overload exact match、decl init forms、struct/array metadata、unsupported feature diagnostics。

9. 端到端 fixtures 首批目标？
   - `int_misc.logic.cpp`、`inline_misc.logic.cpp`、array ports fixture、s7 complex aggregate fixture。

10. 是否继续支持 V2 之外的端口/IR 导出格式？
    - V2 仅维护 `rtl`、`beir` 和 `portmeta`；差分脚本使用 `portmeta`。

11. 与原 AST 并存策略？
    - 源码目录独立 `src/s0ast`，namespace `pred::s0ast`；不依赖其他 AST builder。

12. CMake 接入边界？
    - 让cmake自动识别接入即可。

13. 失败时 fallback 到旧 AST 吗？
    - 不 fallback。V2 默认路径失败就报告 S0V2 diagnostic，避免静默回到旧解析语义。

14. 迁移期间如何保持已有阶段兼容？
    - S0V2 到 S1 的输出尽量一次收敛；后续 S3-S11 不应感知旧/新 AST 来源。
