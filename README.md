# dicescript-c-lib

`dicescript-c-lib` 是 DiceScript 的 C99 兼容实现，供 Dice!Next 及其他本地程序嵌入。库本身不依赖 C++ ABI，也不需要 Go 运行时。

本项目独立维护解析器、求值器、运行时限制和测试。语言语义参考并移植自 [sealdice/dicescript](https://github.com/sealdice/dicescript)，当前固定兼容基线为提交 `c4c99fe00cb67463825a54c8c6e928390501a849`。这不是把 Go 源码直接嵌入程序，而是面向相同行为的 C99 实现。

## 已实现能力

语言与运行时：

- 整数、浮点数、字符串、布尔值、空值、数组、字典、区间和 UTF-8 索引/切片
- 算术、比较、逻辑、位运算、空值合并、三元表达式与从左到右连续 `d` 运算
- 变量、属性与下标读写，持久上下文、原始值读取和 computed value
- `if` / `else`、`while`、`break`、`continue`、`return` 和递归函数
- 普通字符串、控制字符字符串与 DiceScript 模板字符串/模板代码块
- 数组、字典、computed 的原型方法以及内建转换、加载、存储和数值函数

掷骰与扩展：

- 普通骰，`kh`、`kl`、`dh`、`dl`、`min`、`max` 和中文优势/劣势
- COC 奖励骰/惩罚骰、Fate、WOD、DoubleCross，可分别启用
- 默认骰面表达式、最小/最大出目模式和可注入随机源
- 自定义骰解析/求值回调、原生函数、原生对象和 `^st` 回调
- 与上游一致的 tagged VMValue JSON，可保存 computed、数组、字典和函数
- 源码前缀执行、`matched` / `rest`、嵌套骰与 computed 计算过程

安全与嵌入：

- 解析和求值分离；仅验证语法不会消耗随机数
- 骰子数量、爆骰轮数、AST 节点、执行步数、容器大小、递归深度和上下文内存限制
- MSVC 与 GCC/MinGW 回归测试；源码保持 C99，可由 Linux/macOS 工具链直接构建

项目兼容的是 DiceScript 的语言与运行语义，不复制上游 Go 实现的字节码、调试器和 Go 类型 API。宿主通过本项目公开的 C ABI 接入。

## 构建

```bash
cmake -S . -B build -DDICESCRIPT_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows、Linux 和 macOS 都使用同一套 C99 源码。生成后的 PEG 解析器已提交到仓库，普通使用者不需要安装生成工具。

## C API 示例

轻量数值接口适合无状态的表达式探测和回退链：

```c
#include <dicescript/dicescript.h>

dicescript_options options;
dicescript_result result;

dicescript_default_options(&options);
if (dicescript_eval("4d6kh3", &options, &result)) {
    printf("%s\n", result.detail);
}
```

完整语言使用持久上下文：

```c
dicescript_runtime_options options;
dicescript_script_result result;
dicescript_context *ctx;

dicescript_default_runtime_options(&options);
ctx = dicescript_context_create(&options);

if (ctx != NULL &&
    dicescript_context_run_complete(
        ctx,
        "func fib(n) { if n <= 1 { return n }; return fib(n-1)+fib(n-2) }; fib(10)",
        &result)) {
    printf("%s\n", result.text);
}

dicescript_context_destroy(ctx);
```

`dicescript_context_run()` 与上游 `Context.Run` 一样执行最长有效前缀，并在 `matched` / `rest` 中返回匹配部分和剩余文本。需要拒绝尾随内容时使用 `dicescript_context_run_complete()`；只允许单个表达式时使用 `dicescript_context_eval()`。

`dicescript_context_validate_expression()` 与 `dicescript_context_validate_script()` 只解析完整输入，不执行代码、不读取或写入宿主变量，也不消耗随机数；`dicescript_context_validate_expression_prefix()` 以相同方式识别最长表达式前缀并返回 `matched/rest`。完整 VM 的 `dicescript_script_result` 同时返回骰子总数和最多 256 个样本，方便宿主继续进行投骰统计。

`dicescript_validate()` 只验证轻量数值表达式且不会调用随机数回调。错误通过 `dicescript_error_kind` 区分为语法、求值和资源限制错误，方便宿主只在语法不支持时尝试下一个引擎。

## 重新生成解析器

语法源位于 `src/dicescript_parser.peg` 和 `src/dicescript_vm_parser.peg`。维护者可使用 [PackCC](https://github.com/arithy/packcc) 同时重新生成两个解析器：

```powershell
.\tools\regenerate-parser.ps1 -Packcc C:\path\to\packcc.exe
```

请同时提交 `.peg`、生成后的 `.c` 和 `.h`，并在 MSVC 与 GCC/Clang 工具链上运行测试。

## 许可证

本项目按 [Apache License 2.0](LICENSE) 发布。派生来源和第三方声明见 [NOTICE](NOTICE)。
