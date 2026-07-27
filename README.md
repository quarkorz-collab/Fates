# Fates：Finding Algebraic Targets via Expression Search

Fates 根据给定的目标值，搜索由数字、常数和运算符组成的表达式，并按复杂度和误差输出结果。程序也支持含变量 `x` 的方程搜索。

当前版本：`1.0`

项目包含两个入口：

- `fates` / `fates.exe`：命令行程序；
- `fates-web.exe`：本地网页界面，调用同目录下的 `fates.exe`。

`pries` / `pries.exe` 是为旧脚本保留的兼容名称，与 `fates` 使用同一份程序代码。

## 功能

- C++20 实现，支持 Linux、macOS 和 Windows；
- 分层搜索、beam 限制、二元组合预算和数值去重；
- 常量表达式和方程两种搜索模式；
- 自定义数字、常数、运算符、符号次数和叶子顺序；
- 双向搜索、递归逆模板、深层组合和可选的遗传搜索；
- 普通文本、LaTeX、JSON 和实时结果输出；
- UTF-8 参数文件和源码扩展接口；
- 内置本地网页界面，静态资源包含 KaTeX，无需联网渲染公式。

搜索是有界的。`--beam`、`--pairs`、数值去重和各扩展阶段的预算都会影响结果，程序不保证枚举给定复杂度内的全部表达式。

## 构建

### CMake

需要支持 C++20 的编译器和 CMake 3.20 或更高版本。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

Windows 使用 Visual Studio 生成器时，可执行文件通常位于 `build/Release/`；使用 Ninja 或单配置生成器时通常位于 `build/`。

CMake 默认生成不强制特定 SIMD 指令集的通用版本。需要在支持 AVX2 的机器上获得更高性能时，可以显式启用：

```bash
cmake -S . -B build-avx2 -DCMAKE_BUILD_TYPE=Release \
  -DFATES_ENABLE_AVX2=ON
cmake --build build-avx2 --config Release --parallel
```

启用后的二进制要求运行机器支持 AVX2。

### Make

Linux 和 macOS 也可以直接使用 Make：

```bash
make -j
./fates --self-test
```

`make NATIVE=1` 会加入本机指令集优化，生成的程序不适合复制到不同 CPU 上运行。

### Windows 和 Linux PGO

安装带“使用 C++ 的桌面开发”组件的 Visual Studio 后，可以直接生成 MSVC PGO 版本：

```powershell
.\scripts\build-pgo-windows.ps1 -EnableAVX2
```

脚本会自动查找 MSVC x64 工具链，依次完成插桩编译、代表性搜索训练、配置文件合并、优化链接和自测。默认输出到 `artifacts/windows-x64-pgo-avx2/`。不加 `-EnableAVX2` 会生成通用 CPU 版本；`-Training quick` 可缩短训练时间，默认 `balanced` 会额外覆盖 portfolio 搜索路径。

Linux 上使用 GCC 构建相同训练标准的 PGO 版本：

```bash
bash scripts/build-pgo-linux.sh --enable-avx2 --training balanced
```

默认输出到 `artifacts/linux-x64-pgo-avx2/`。正式发布构建固定使用 `balanced`。Windows 和 Linux 都读取 [`scripts/pgo-workloads.json`](scripts/pgo-workloads.json) 中的 `release-balanced-v1`：版本启动、自测、确定性搜索、遗传搜索、方程搜索和 portfolio 搜索六项训练负载均与最终性能优化时使用的负载一致。`build-info.txt` 会记录 profile 名和训练文件 SHA-256，便于核对发布包没有使用缩减训练。

PGO 配置文件与生成它的源码、编译器版本和编译选项绑定，不能可靠地跨版本复用，因此脚本每次都从当前源码重新训练。

### VS Code

请用 **File > Open Folder** 打开整个项目目录，使仓库内的 `.vscode` C++20 配置生效；仅单独打开 `src/fates.cpp` 时，VS Code 不会读取该配置。若仍看到 `std::set::contains`、`std::bit_cast`、`std::rotl`、`std::countl_zero` 或 `std::numbers` 不存在等提示，请在命令面板依次运行 `C/C++: Reset IntelliSense Database` 和 `Developer: Reload Window`。这些提示通常来自扩展沿用旧的 C++17 数据库，不是编译错误。若 C/C++ 扩展的中文诊断本身乱码，可临时通过 `Configure Display Language` 切换到 English；这只改变编辑器诊断语言，不影响源码编码或程序输出。

### GitHub Actions

仓库包含两个工作流：

- `CI`：每次 push 和 pull request 在 Windows、Linux、macOS 上构建并运行测试；
- `Build release artifacts`：可在 GitHub 的 Actions 页面手动点击 **Run workflow**，也会在推送 `v*` 标签时运行。

发布构建会生成五个可下载 artifact，每个 artifact 内含可直接上传 Releases 的归档和归档 SHA-256：

```text
fates-windows-x64-portable.zip
fates-windows-x64-pgo-avx2.zip
fates-linux-x64-portable.tar.gz
fates-linux-x64-pgo-avx2.tar.gz
fates-macos-portable.tar.gz
```

PGO Windows 包同时包含 `fates-web.exe`。每个包包含许可证、第三方声明、构建信息和 SHA-256 校验文件，不需要配置仓库 secret。Linux 发布任务固定使用 Ubuntu 22.04，以避免 `ubuntu-latest` 漂移到更新 glibc 后降低二进制兼容性；本地脚本则会链接当前系统的 glibc。推送 `v*` 标签时，工作流会在所有包验证成功后自动创建对应 GitHub Release；手动运行工作流时只生成 Actions artifacts。

项目默认使用 `third_party/unordered_dense`。如需使用标准库容器：

```bash
cmake -S . -B build-std -DCMAKE_BUILD_TYPE=Release \
  -DFATES_USE_STD_CONTAINERS=ON
cmake --build build-std --config Release --parallel
```

## 快速开始

使用 `sqrt` 构造 `sqrt(2)`：

```bash
./fates 1.4142135623730951 \
  --digits 2 \
  --constants none \
  --ops sqrt \
  --max-cost 2
```

Windows PowerShell：

```powershell
.\fates.exe 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2
```

只使用有理数逼近 π：

```bash
./fates 3.141592653589793 \
  --constants none \
  --digits 123456789 \
  --max-literal-len 3 \
  --max-integer 999 \
  --ops / \
  --max-cost 7
```

将结果写成 JSON：

```bash
./fates 2.5063 --json > result.json
```

## 常用参数

### 数字、常数和运算符

```text
--digits 123456789
--constants pi,e,phi
--constants none
--constant G=0.915965594177219:2
--ops '+,-,*,/,^,sqrt,ln'
```

`--digits` 指定可用的数字，数字可以重复使用。默认最长字面量长度为 2，整数字面量上限为 25；可用 `--max-literal-len` 和 `--max-integer` 调整。

运算符和常数后的 `:N` 是复杂度成本。例如 `sqrt:1,ln:2` 分别使用成本 1 和 2。`+`、`-`、`*`、`/` 的默认成本为 1，`^` 的默认成本为 2。

### 符号次数和顺序

`--symbol-count` 按完整原子文本限制出现次数：

```powershell
.\fates.exe 2 --digits= --constants pi --ops '+,/' --max-cost 7 `
  --symbol-count pi=4

.\fates.exe 24 --digits 1234 --max-literal-len 1 --constants none `
  --ops '+,-,*,/' --max-cost 7 `
  --symbol-count 1=1 --symbol-count 2=1 `
  --symbol-count 3=1 --symbol-count 4=1
```

`NAME=N` 表示恰好出现 `N` 次，`NAME=MIN:MAX` 表示闭区间。上限可以写成 `inf`、`infinity` 或 `*`。

`--symbol-order` 约束指定符号在表达式叶子中的顺序：

```powershell
.\fates.exe TARGET --symbol-order '1,1,4,5,1,4'
```

### 误差区间

`--error-range` 过滤有符号误差。常量模式中误差是 `value - TARGET`，方程模式中误差是根偏移：

```powershell
.\fates.exe 1.4142135623730951 --error-range '(0,1)'
.\fates.exe 1.4142135623730951 --error-range '[0,1)'
```

PowerShell 中建议给区间加引号，以保留括号的开闭含义。

### 参数文件

复杂命令可以放在 UTF-8 参数文件中：

```powershell
.\fates.exe --args-file examples\24-point.args
.\fates.exe '@examples/24-point.args'
```

参数文件支持注释、空字符串和最多 8 层嵌套。相对路径以当前参数文件所在目录为基准。

### 方程搜索

启用 `--equations` 后，程序使用变量 `x` 搜索方程：

```bash
./fates 1.4142135623730951 \
  --equations --digits 2 --constants none --ops '^' \
  --max-cost 5 --mode nearest
```

`--equation-quality` 可选 `strict`、`local` 或 `off`。默认模式会对候选根重新求值，并检查邻域定义域和导数。方程模式不支持遗传阶段、递归逆模板和深层组合。

### 搜索扩展

以下选项默认关闭：

```text
--inverse-depth 1 --inverse-beam 64 --inverse-budget 1000000
--deep-rounds 1 --deep-beam 100
--pareto-slots 2 --pareto-extra 750
--genetic --genetic-population 4096 --genetic-generations 64
```

这些阶段会增加搜索时间或内存。`--search-mode portfolio` 会组合部分扩展；需要可复现结果时，应固定目标、全部参数、线程数和 `--genetic-seed`。

## 输出和实时结果

常用输出选项：

```text
--mode pareto       按复杂度和误差输出前沿
--mode nearest      按误差输出结果
--results N         输出 N 条结果
--json              输出 JSON
--latex             在普通输出中显示 LaTeX
--live              持续输出当前结果
--live-top N        指定实时结果数量
--live-json         以 JSON 事件输出实时结果
--dry-run           只解析参数，不开始搜索
--no-stats          关闭最终统计
```

JSON 结果同时包含纯文本表达式和 LaTeX 表示。实时 JSON 事件写入标准错误，最终 JSON 写入标准输出，两个通道可以分别重定向。

## 本地网页界面

从源码运行：

```powershell
python .\frontend\fates_web.py --fates .\fates.exe
```

服务器只监听回环地址，默认选择空闲端口，并在终端打印访问地址。也可以指定端口或关闭自动打开浏览器：

```powershell
python .\frontend\fates_web.py --port 9000 --no-browser
```

生成独立的 Windows 程序：

```powershell
.\frontend\build_web.ps1
```

脚本将 `fates-web.exe` 写入项目根目录。运行时把它放在 `fates.exe` 旁边即可。

## 参数参考

| 参数 | 说明 | 默认值 |
|---|---|---:|
| `TARGET` | 有限浮点目标值 | 普通搜索必填 |
| `--max-cost N` | 最大表达式复杂度 | `10` |
| `--beam N` | 每个复杂度层的候选上限 | `3000` |
| `--pairs N` | 每层二元组合预算 | `4000000` |
| `--threads N` | 工作线程数；`0` 使用硬件并发数 | 硬件并发数 |
| `--task-chunks N` | 每个组合分区的任务块数 | `64` |
| `--value-bits N` | 数值分桶保留的尾数位 | `42` |
| `--value-prune MODE` | `bucket` 或 `exact` | `bucket` |
| `--side-cost N` | 双向搜索的单边成本；`0` 自动选择 | 自动 |
| `--no-bidirectional` | 使用完整逐层搜索 | 关闭 |
| `--deep-rounds N` | 深层组合轮数 | `0` |
| `--deep-beam N` | 每轮新增候选上限 | 自动 |
| `--deep-frontier N` | 每成本参与深层组合的输入上限 | 自动 |
| `--inverse-neighbors N` | 逆值查询的近邻数 | `5` |
| `--inverse-depth N` | 递归逆模板深度 | `0` |
| `--inverse-beam N` | 每个递归请求的候选数 | `64` |
| `--inverse-budget N` | 递归逆阶段的组合预算 | `1000000` |
| `--pareto-slots N` | 每个数值桶的结构候选槽数 | `1` |
| `--pareto-extra N` | 结构候选额外预算 | 自动 |
| `--equations` | 启用方程搜索 | 关闭 |
| `--equation-neighbors N` | 方程配对近邻数 | `64` |
| `--equation-search MODE` | `stable`、`wide` 或 `exhaustive` | `stable` |
| `--equation-quality MODE` | `strict`、`local` 或 `off` | `strict` |
| `--genetic` | 启用遗传混合阶段 | 关闭 |
| `--search-mode MODE` | `deterministic`、`genetic`、`hybrid` 或 `portfolio` | `deterministic` |
| `--genetic-population N` | 种群规模和每代试验数 | `4096` |
| `--genetic-generations N` | 最大代数 | `64` |
| `--genetic-seed N` | 随机种子 | `2611923443488327891` |
| `--genetic-elegance X` | 复杂度惩罚 | `0.35` |
| `--genetic-repair X` | 局部修复概率 | `0.15` |
| `--genetic-repair-depth N` | 局部修复深度 | `3` |
| `--genetic-crossover X` | 子树交叉概率 | `0.25` |
| `--genetic-novelty X` | 结构代表比例 | `0.20` |
| `--genetic-tournament N` | 选亲锦标赛规模 | `4` |
| `--error-range RANGE` | 有符号误差区间 | `(-inf,inf)` |
| `--epsilon X` | 达到该误差后停止 | `1e-12` |
| `--no-stop` | 不因误差阈值停止 | 关闭 |
| `--results N` | 输出结果数 | `20` |
| `--mode MODE` | `pareto` 或 `nearest` | `pareto` |
| `--live` | 输出实时结果 | 关闭 |
| `--live-top N` | 实时结果数量 | 关闭 |
| `--live-interval SEC` | 实时刷新间隔 | `2` |
| `--live-json` | 实时结果使用 JSON 事件 | 关闭 |
| `--latex` | 普通输出显示 LaTeX | 关闭 |
| `--json` | 输出 JSON | 关闭 |
| `--dry-run` | 只解析参数 | 关闭 |
| `--verbose` | 输出逐层统计 | 关闭 |
| `--no-stats` | 关闭最终统计 | 关闭 |
| `--list-symbols` | 列出运算符和常数 | 关闭 |
| `--self-test` | 运行内置测试 | 关闭 |
| `--version` | 显示程序版本 | 关闭 |
| `-h, --help` | 显示帮助 | 关闭 |

程序还支持 `--max-literal-len`、`--max-integer`、`--digit-cost`、`--constants`、`--constant`、`--symbol-count`、`--symbol-order`、`--ops`、`--max-abs`、`--max-exponent`、`--max-trig-arg`、`--max-atoms` 和 `--args-file`。运行 `fates --help` 或 `fates --list-symbols --json` 查看当前二进制的完整目录。

## 实现概览

- 表达式按复杂度分层生成，候选使用紧凑 AST 索引保存，最后一步才渲染文本；
- 数值分桶、成本上限和组合预算共同控制候选数量；
- 默认模式先生成单边表达式，再通过 meet-in-the-middle 合并；
- 方程模式额外保存目标点处的导数，并对候选根做数值复核；
- 源码扩展可以注册运算和结构约束，详见 [`docs/EXTENDING.md`](docs/EXTENDING.md)。

## 测试和基准

核心自测：

```bash
./fates --self-test
```

CMake 构建使用 CTest 注册同一项自测。完整命令行回归位于 `tests/smoke.sh`；容器后端比较位于 `tests/benchmark_containers.ps1`。基准结果会受到处理器、编译器、线程数和系统负载影响，不应直接作为跨机器性能结论。

GitHub Actions 使用相同的 CMake/CTest 入口，Linux 任务还会执行完整 `smoke.sh`。性能相关改动的提交要求见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。

## 限制

- 搜索使用 IEEE 754 `double`；“零误差”表示浮点计算结果为零，不代表数学证明；
- 负底数的非整数幂、非法函数定义域和溢出中间值会被过滤；
- 阶乘只接受接近非负整数且不大于 170 的输入；
- 方程结果取决于初值、迭代次数和局部收敛情况；
- 增大 `--beam`、`--pairs` 或 `--value-bits` 会增加内存和计算时间。

## 目录

```text
Fates/
├── .github/
│   ├── dependabot.yml
│   └── workflows/
│       ├── ci.yml
│       └── build-release.yml
├── CMakeLists.txt
├── CONTRIBUTING.md
├── Makefile
├── LICENSE
├── README.md
├── THIRD_PARTY_NOTICES.md
├── docs/
│   └── EXTENDING.md
├── examples/
│   └── 24-point.args
├── src/
│   ├── extension_api.h
│   ├── fates.cpp
│   ├── fast_containers.h
│   └── user_extensions.h
├── frontend/
│   ├── fates_web.py
│   ├── build_web.ps1
│   ├── requirements-build.txt
│   └── static/
├── scripts/
│   ├── build-pgo-linux.sh
│   ├── build-pgo-windows.ps1
│   ├── pgo-workloads.json
│   └── run-pgo-training.py
├── third_party/
│   └── unordered_dense/
└── tests/
    ├── benchmark_containers.ps1
    └── smoke.sh
```

## 许可证

Fates 本体使用 MIT 许可证。`ankerl::unordered_dense` 和网页端 KaTeX 也使用 MIT 许可证，许可证文本保存在各自目录中；汇总信息见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
