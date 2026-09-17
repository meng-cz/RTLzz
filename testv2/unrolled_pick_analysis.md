# 循环 pick 写入回归

相关用例已全部合并到 `fixtures/int_misc.logic.cpp`，通过 `pick_*` 输出覆盖：

- `pick_constant`：`Int<128> x = 0; for (int i = 0; i < 4; i++) x.pick<32>(i * 32) = Int<32>(i);`，预期 `0x00000003000000020000000100000000`。
- `pick_static`：运行时数据，循环常量索引读取和写入。
- `pick_dynamic`、`pick_bit`：运行时索引位段/单 bit 写入，防止索引和值交换。
- `pick_ordered`：RHS/LHS 自增，验证赋值右侧先求值。
- `pick_overlap`、`pick_edges`、`pick_full`：`i * 4` 重叠写入、首尾及整宽覆盖。

在 RTLZZ 根目录运行：

```bash
python3 scripts/check_unrolled_pick.py --build-dir <build>
python3 scripts/differential_rtl.py testv2/fixtures/int_misc.logic.cpp --top hls_main --build-dir <build> --cases 100 --beopt none
```

曾发现的根因及修复：S3 将动态写入的 index/value 操作数交换，现保留求值顺序并按 base/index/value 输出；后端缺少常量索引静态化，现转为 Slice/Concat；Concat 缺少常量传播且位宽缩窄改变拼接布局，现已补齐位值传播并保持每项的位宽。

结构检查仅遍历对应 pick 输出的依赖，允许 int_misc 原有用例保留运行时动态读取。数值差分覆盖整个 int_misc。

## 合并验证记录

新增 8 个 pick 输出的结构检查及 100 组独立数值比较通过。完整 int_misc 差分在 case=3 的既有 `stdmix_mul_s8` 输出失败：a=83、b=201 时 C++=60971、RTL=16683。关闭优化也出现相同差异；同一输入在合并前 fixture 中通过，提示编译上下文相关的有符号乘法问题。检查脚本保持失败返回，不隐藏该差异；本次仅合并测试，不扩展修改编译器。
