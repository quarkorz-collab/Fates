# Fates Web Server

网页服务器为 Fates 提供本地界面，支持 Linux 和 Windows。它只接受回环地址，并将经过校验的参数直接传给原生 Fates 引擎（Linux 的 `fates`、Windows 的 `fates.exe`），不通过命令 shell 执行。

启动时，服务器会从引擎的 `--list-symbols --json` 读取常数和运算符目录，因此源码扩展会出现在网页表单中。KaTeX、样式和字体已放在 `static/vendor/katex`，独立程序也包含这些资源，离线时可以渲染公式。

界面覆盖命令行的确定性、方程、遗传、PSLQ、e-graph、MCTS 和 portfolio 配置。高级阶段使用“自动 / 启用 / 排除”三态选择：自动状态在 portfolio 中启用，排除状态会生成对应的 `--no-*` 参数。命令框支持导入这些选项，包括独立的 `--result-value-bits` 结果去重精度；结果可随时在 KaTeX 和纯文本之间切换并复制 LaTeX 源码。

“原子、运算与约束”里的“常数合计次数”每行输入一个分组，如 `pi,e,phi=2:4`；只允许当前启用的内置或自定义常数，区间上下界都包含，上界可写 `inf`。各行会生成独立的 `--constant-count` 参数，也可从命令框导入。方程模式将等号两侧的出现次数相加。

方程的“估计根”列保留后端返回的完整浮点精度，实时和最终榜单一致，避免把 `777777.0000000017` 显示成 `777777`。其他数值列仍使用紧凑格式。

## 使用发布包

Linux 发布包同时包含 `fates` 和 `fates-web`，解压后在该目录运行：

```bash
./fates-web
```

独立程序无需安装 Python，也无需联网下载前端资源。`fates-web` 必须与原生 `fates` 放在同一目录，或通过 `--fates PATH` 指定引擎。tar.gz 会保留执行权限；如果复制文件时权限丢失，运行 `chmod +x fates fates-web`。

Windows 使用 `fates-web.exe`，与 `fates.exe` 放在同一目录。两个平台都默认自动打开浏览器，在终端显示实际地址；自动打开失败时也可以手动访问。

## 从源码运行

需要 Python 3.10 或更高版本，服务器运行时只使用 Python 标准库。先构建对应平台的 Fates 引擎；若使用 CMake 的默认输出目录，Linux 上运行：

```bash
python3 frontend/fates_web.py --fates ./build/fates
```

Windows（引擎在项目根目录时）：

```powershell
python .\frontend\fates_web.py --fates .\fates.exe
```

参数：

```text
--host HOST      仅允许回环地址；默认 127.0.0.1
--port PORT      监听端口；0 表示自动选择，默认 0
--fates PATH     指定原生 Fates 可执行文件路径
--no-browser     启动后不打开浏览器
--verbose        输出 HTTP 访问日志
```

省略 `--fates` 时，独立程序依次查找自身目录、当前目录和 `PATH`；源码版首先查找项目根目录。Linux 自动查找不会选中同目录的 Windows `fates.exe`（包括 WSL）。显式给出的路径无效或不可执行时会报错，不会回退到其他引擎。

命令框根据**服务器所在平台**生成和导入命令：Linux 使用 `./fates` 和 Bash/POSIX 引号、反斜杠续行，Windows 保留 PowerShell 格式和反引号续行。支持空参数、包含空格或单引号的路径和 `--name=value`；只解析参数，不执行 shell 展开、管道或重定向。参数文件的相对路径以引擎所在目录为准。

## Linux 无桌面或远程使用

在 Linux 主机上启动：

```bash
./fates-web --port 9000 --no-browser
```

若浏览器在另一台机器上，在那台机器建立 SSH 隧道：

```bash
ssh -N -L 9000:127.0.0.1:9000 user@linux-host
```

然后访问 `http://127.0.0.1:9000/`。不需要也不支持绑定 `0.0.0.0`。Ctrl+C 或 SIGTERM 会停止服务器，并取消、回收正在运行的搜索；任务忽略 SIGTERM 时会升级为强制终止。

## 构建 Linux 程序

需要 Linux、Python 3.10+（含 pip 和 Python 共享库）以及 binutils。Debian/Ubuntu 可安装 `python3 python3-pip binutils`。在已经有 `build/fates` 的项目中运行：

```bash
bash frontend/build_web.sh --output build
./build/fates-web
```

脚本在独立临时目录安装固定版本的 PyInstaller 并构建单文件程序，完成后清理该临时目录，不向系统 Python 安装构建依赖。省略 `--output` 时输出到项目根目录；`--python /path/to/python3` 或 `PYTHON` 可选择解释器。构建本身需要下载依赖，生成的程序运行时不需要网络。

与 AVX2 PGO 引擎一起构建：

```bash
bash scripts/build-pgo-linux.sh --enable-avx2 --training balanced
bash frontend/build_web.sh --output artifacts/linux-x64-pgo-avx2
./artifacts/linux-x64-pgo-avx2/fates-web
```

PyInstaller 不交叉编译：应在目标架构的 Linux 上构建（Windows 开发机可用 WSL），生成文件依赖构建系统的 glibc，不能直接用于 Alpine/musl。GitHub Linux 发布包在 Ubuntu 22.04 x86-64 上构建；本地在较新系统构建的程序不保证能在旧发行版运行。AVX2 包仍要求 CPU 支持 AVX2，通用包没有此要求。

## 构建 Windows 程序

`build_web.ps1` 会在临时目录安装 PyInstaller，构建完成后删除临时目录，并把结果写入项目根目录：

```powershell
.\frontend\build_web.ps1
```

生成的 `fates-web` / `fates-web.exe` 属于构建产物，不应提交到源码仓库；项目根目录的 `.gitignore` 已包含相应规则。
构建依赖固定在 `requirements-build.txt`，由 Dependabot 定期检查更新。

## 发布与验证

GitHub Actions 的 `Build release artifacts` 工作流会将网页程序加入 Windows AVX2 PGO 包，以及 Linux 的通用包和 AVX2 PGO 包；Linux 包内的校验和覆盖 `fates-web`、引擎和 `WEBUI.md`。正式 PGO 发布仍使用 `release-balanced-v3` 的 `balanced` 训练集。

跨平台单测和真实引擎检查：

```bash
python3 -m unittest discover -s tests -p 'test_web*.py'
node tests/test_web_commands.cjs
node tests/test_web_numbers.cjs
node tests/test_web_constant_counts.cjs
python3 tests/check_web.py --bin build/fates
python3 tests/check_web.py --bin build/fates --web build/fates-web
```

最后一项会复制独立程序和引擎到包含空格的临时目录，从另一工作目录启动，检查自动发现、离线资源、普通/方程搜索与 CLI 结果一致、实时 JSON、取消任务和 Linux SIGTERM 清理。测试不打开浏览器，不修改被测程序。
