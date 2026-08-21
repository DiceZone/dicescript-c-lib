# dicescript-c-lib

`dicescript-c-lib` 是由 Dice!Next 维护的 DiceScript C99 兼容库。它将 DiceScript 的常用数值表达式与 TRPG 掷骰语义封装为稳定、无 C++ ABI 依赖的 C 接口，供 Dice!Next 及其他本地程序嵌入。

本项目不是等待上游发布的占位仓库：解析器、求值器、限制机制和测试均在此仓库独立维护。语法设计参考并移植自 [sealdice/dicescript](https://github.com/sealdice/dicescript)，当前兼容基线为提交 `c4c99fe00cb67463825a54c8c6e928390501a849`。

## V1 范围

- 整数、浮点数、括号、算术、比较、逻辑、位运算与三元表达式
- 普通骰及连续 `d` 运算，例如 `10d10d1` 按同级运算符从左到右求值
- `kh`、`kl`、`dh`、`dl`、`min`、`max`，以及中文优势/劣势
- COC 奖励骰/惩罚骰、Fate、WOD、DoubleCross
- 解析验证与实际求值分离；探测语法不会消耗随机数
- 掷骰数量、爆骰轮数、AST 节点数与递归深度限制
- 可注入随机源，便于宿主接入安全随机数和编写确定性测试

V1 暂不包含完整 DiceScript 语言中的变量、字符串、数组、字典、函数、语句和模板系统。这些语法会明确返回“不支持的语法”，不会被误判为求值错误。

## 构建

```bash
cmake -S . -B build -DDICESCRIPT_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows、Linux 和 macOS 都使用同一套 C99 源码。生成后的 PEG 解析器已提交到仓库，普通使用者不需要安装生成工具。

## C API

```c
#include <dicescript/dicescript.h>

dicescript_options options;
dicescript_result result;

dicescript_default_options(&options);
if (dicescript_eval("4d6kh3", &options, &result)) {
    printf("%s\n", result.detail);
}
```

调用 `dicescript_validate()` 可只验证完整表达式；该函数不会调用随机数回调。错误通过 `dicescript_error_kind` 区分为不支持的语法、求值错误和安全限制错误，方便宿主仅在语法不支持时尝试下一个引擎。

## 重新生成解析器

语法源位于 `src/dicescript_parser.peg`。维护者可使用 [PackCC](https://github.com/arithy/packcc) 重新生成：

```powershell
.\tools\regenerate-parser.ps1 -Packcc C:\path\to\packcc.exe
```

## 许可证

本项目按 [Apache License 2.0](LICENSE) 发布。派生来源和第三方声明见 [NOTICE](NOTICE)。
