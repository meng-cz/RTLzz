# RTLzz 可综合 C++ 语法子集

本文定义当前 RTLzz V2 pipeline 实际接受并可 lowering 到 RTL 的 C++ 子集边界。源码需先通过 libclang C++20 解析；未显式包含 `fixint.hpp` 的输入会由 RTLzz 在解析时补入该头文件。

## 入口与端口

1. 顶层函数必须是源文件中的一个 `void name()` 形式函数：返回 `void`，没有源码参数。
2. RTL 端口由同一源文件的文件级全局变量声明，并用 `#pragma input_port name` 或 `#pragma output_port name` 指定方向。
3. 每个文件级全局变量都必须有且只有一个匹配的端口 pragma；无变量的 pragma、重复 pragma、未标注全局变量都会报错。
4. 端口变量不能带初始化器，不能是 `const`、指针、引用或 struct/class 类型。
5. 输入端口按值传入 lowering；输出端口在内部按 mutable reference 形态表示。
6. 顶层、helper、函数模板特化和 lambda 可直接读写全局端口。

## 标量类型

1. 支持 `bool`，lowering 为 1 bit 布尔硬件值。
2. 支持 `Int<N>`，其中 `N > 0`，用于任意定宽无符号硬件整数存储。
3. 支持 `Int<N>::sint()` 形成的有符号操作视图；存储仍是 `Int<N>`，signedness 只影响对应操作解释。
4. 支持 64 位以内内置整数和 typedef：`char`、`signed char`、`unsigned char`、`short`、`unsigned short`、`int`、`unsigned int`、`long`、`unsigned long`、`long long`、`unsigned long long`、`int8_t`/`uint8_t`、`int16_t`/`uint16_t`、`int32_t`/`uint32_t`、`int64_t`/`uint64_t` 以及 `std::int*_t`/`std::uint*_t` 对应形式。
5. 支持整型枚举值；enum 类型按其整数 underlying type 转成定宽硬件整数。
6. **不支持浮点类型**。

## 聚合类型

1. 支持静态维度数组：`std::array<T, N>` 和 C-style **常量**数组 `T[N]`。
2. 支持多维静态数组，表示为嵌套 `std::array` 或 C-style 多维**常量**数组。
3. 数组元素类型可以是受支持标量、数组或受支持 struct/class 聚合。
4. 支持非空 struct/class 作为局部变量、helper/lambda 参数、helper/lambda 返回值和中间表达式。
5. struct/class 字段类型可以是受支持标量、数组或 struct/class 聚合。
6. struct/class 字段不能是指针或引用。
7. 空 struct/class 不支持。
8. **顶层端口不支持 struct/class 聚合；支持的数组端口最终元素必须是 `bool`、`Int<N>`**。

## 声明与初始化

1. 支持局部变量声明带初始化器：`T x = expr;`、`T x(expr);`、`T x{...};`。
2. 支持局部变量声明不带初始化器：`T x;`。
3. 未初始化局部变量可以声明；无初始化的数组和聚合不会隐式补零。
4. 读取未被赋值过的变量会报错。
5. 未初始化读取包括显式读取变量，以及动态数组写需要读取目标数组旧值时产生的隐式读取。
6. `Int<N> x;` 当前保留 fixint 默认构造路径；需要明确全零初始化数组或聚合时应写 `= {}`。
7. 支持 `= {}` 对数组、struct/class 及数组-struct 复合聚合做显式全零/默认聚合初始化。
8. 支持聚合 positional 初始化、copy-list 初始化和 C++ designated 初始化，例如 `Pair{a, b}`、`Pair p = {...}`、`Pair{.lo = a}`。
9. 支持聚合 copy 和同形状聚合赋值。
10. 支持函数内 `const`/`constexpr` 整型常量和 enum 常量参与模板参数、位段边界、静态索引等编译期求值。
11. 支持 `static const` 局部数组作为查表常量；其他 static 局部声明不支持。

## 函数

1. 支持同源文件内普通 helper 函数。
2. helper 返回类型可以是 `void`、受支持标量、受支持数组或受支持 struct/class 聚合。
3. helper 参数可以按值传递、按 `const T&` 传递，或按 mutable `T&` 传递以表达被调用方写回。
4. helper 参数不支持指针和 rvalue reference。
5. 支持按参数类型和数量解析函数重载；完全重复签名不支持。
6. 支持 helper 调用 helper，以及多级调用。
7. 不支持递归调用图。
8. 非 `void` helper/lambda 应显式返回受支持类型的值。

## 函数模板

1. 支持函数模板的特化。
2. 支持 integral template parameter。
3. 模板实参可使用整数字面量、enum 常量、函数内 `constexpr` 整型常量、模板参数本身，以及由这些量组成的常量表达式。
4. 支持在模板 helper 内继续调用其他模板 helper 或 lambda 特化。
5. 不支持类型模板参数或需要运行期决定的模板实参作为硬件语义。

## Lambda

1. 支持局部 lambda。
2. 支持按值捕获`[=]`和按引用`[&]`捕获当前可综合变量。
3. 支持 lambda 读写全局端口。
4. 支持 nested lambda。
5. 支持 lambda 调用 helper、helper 调用 lambda、lambda 调用 lambda。
6. 支持 generic/template lambda 的显式 `operator()<...>` 调用和实际使用特化。
8. lambda 参数遵守 helper 参数同样的传递限制。
9. 不支持无法解析调用目标的函数对象、函数指针或 `std::function`。

## 控制流

1. 支持 `{ ... }` block 和普通词法作用域。
2. 支持 `if` 和 `if/else`。
3. 支持 `for`、`while`、`do while`，但所有循环都必须在 S5 被静态分析并完全展开。
4. 循环条件必须可归约为循环变量与常量边界的简单比较：`<`、`<=`、`>`、`>=`、`==`、`!=`。
5. 循环 induction 变量必须有常量初值，并在回边路径上按固定 affine delta 更新。
6. 循环展开迭代数必须不超过 `--unroll-limit`/`max_iterations_per_loop`。
7. 支持嵌套循环。
8. 支持循环内 `break` 和 `continue`；`break`/`continue` 只能出现在合法循环上下文中。
9. 支持 `switch/case/default`。
10. `switch` 不支持 fall-through；每个 case/default 必须以 `break` 或 `return` 结束。
11. 顶层函数只能 `return;` 或自然结束，不能返回值。

## 表达式与运算

1. 支持 bool literal 和 integer literal。
2. 支持变量引用、字段访问 `a.b` 和数组下标 `a[i]`。
3. 支持普通赋值 `=`。
4. 支持复合赋值 `+=`、`-=`、`*=`、`/=`、`%=`、`&=`、`|=`、`^=`、`<<=`、`>>=`，按读取旧值再赋新值 lowering。
5. 支持一元 `!`、`~`、`+`、`-`。
6. 支持前置/后置 `++`、`--` 作为语句形式，lowering 为加一/减一赋值。
7. 支持算术 `+`、`-`、`*`。
8. 支持 `/` 和 `%` 的无符号常量除数形式；第二操作数必须是常量。
9. 支持位运算 `&`、`|`、`^`。
10. 支持移位 `<<`、`>>`；右移会根据 signed view 选择逻辑右移或算术右移。
11. 支持比较 `==`、`!=`、`<`、`<=`、`>`、`>=`。
12. 支持逻辑 `&&`、`||`、`!`。
13. 支持三目条件表达式 `cond ? a : b`。
14. 支持 C-style cast、`static_cast<T>` 和函数式 cast，用于受支持标量、enum 和可识别聚合临时值。
15. 不支持 comma expression。

## `Int<N>` 与 fixint API

1. 支持 `Int<N>(x)` 构造/转换。
2. 支持 `x.at<Hi, Lo>()` 静态位段读取。
3. 支持 `x.at<Bit>()` 静态单 bit 读取。
4. 支持 `x.at<Hi, Lo>() = value` 静态位段写。
5. 支持 `x.at<Bit>() = value` 静态单 bit 写。
6. `at` 的模板参数可以是整数字面量、模板参数、enum 常量、函数内编译期整型常量，以及这些量的表达式。
7. 支持 `x.pick<N>(idx)` 动态定宽位段读取。
8. 支持 `x.pick(idx)`/动态 bit API 形式读取单 bit。
9. 支持动态位段/bit 写对应的硬件 op。
10. 支持 `x.to<T>()` 转换到受支持目标整数类型。
11. 支持 `x.sint()` 作为有符号操作视图。
12. 支持 `Cat(...)`、`cat(...)`、`concat(...)`。
13. 支持 `Repeat<N>(x)`/`repeat<N>(x)`。
14. 支持 `ReduceOr(x)`/`reduce_or(x)`、`ReduceAnd(x)`/`reduce_and(x)`、`ReduceXor(x)`/`reduce_xor(x)`。
15. 支持 `zext<N>(x)`/`ZExt<N>(x)` 和 `trunc<N>(x)`/`Trunc<N>(x)`。

## 数组与聚合访问 lowering

1. 静态数组下标读取会展平为对应 leaf 读取。
2. 静态数组下标写入会展平为对应 leaf 写入。
3. 动态数组下标读取会 lowering 为 lookup/mux。
4. 动态数组下标写入会 lowering 为 guarded write/lookupwrite。
5. 动态数组写会读取目标数组当前所有元素，因此目标数组所有可能被读 leaf 必须已初始化。
6. struct/class 字段读取会展平为对应 leaf 读取。
7. struct/class 字段写入会展平为对应 leaf 写入。
8. 函数或 lambda 返回的数组/struct 临时值可以直接读取字段或下标。
9. 三目表达式可在同形状聚合之间选择；含动态数组读取的聚合三目目前不支持。

## 明确不在当前子集内

1. `new`、`delete`、`malloc`、`free` 和堆分配。
2. `throw`、`try`、`catch` 和异常控制流。
3. virtual function。
4. 浮点类型和浮点 literal。
5. 指针类型、指针参数、指针字段和指针算术。
6. rvalue reference。
7. 局部非常量引用 alias。
8. 函数指针和 `std::function`。
9. `volatile` 和 `atomic`。
10. I/O 语句或调用，例如 `std::cout`、`std::cin`、`printf`、`scanf`、`fprintf`、`iostream`。
11. 除 `std::array` 外的 STL 容器，例如 `std::vector`、`std::map`、`std::unordered_map`、`std::deque`、`std::list`、`std::set`、`std::queue`。
12. 动态大小数组或未知大小数组。
13. switch fall-through。
14. 递归。
15. legacy proxy carrier 类型。
16. 文件级普通变量作为状态或常量；文件级变量当前只作为端口声明处理。
