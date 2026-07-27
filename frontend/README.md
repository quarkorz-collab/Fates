# Fates Web Server

网页服务器为 Fates 提供本地界面。它只接受回环地址，并将经过校验的参数直接传给 `fates.exe`，不通过命令 shell 执行。

启动时，服务器会从 `fates.exe --list-symbols --json` 读取常数和运算符目录，因此源码扩展会出现在网页表单中。KaTeX、样式和字体已放在 `static/vendor/katex`，离线时也可以渲染公式。

界面覆盖命令行的确定性、方程、遗传、PSLQ、e-graph、MCTS 和 portfolio 配置。高级阶段使用“自动 / 启用 / 排除”三态选择：自动状态在 portfolio 中启用，排除状态会生成对应的 `--no-*` 参数。命令框支持导入这些选项，结果可随时在 KaTeX 和纯文本之间切换并复制 LaTeX 源码。

## 从源码运行

```powershell
python .\frontend\fates_web.py --fates .\fates.exe
```

参数：

```text
--host HOST      仅允许回环地址；默认 127.0.0.1
--port PORT      监听端口；0 表示自动选择，默认 0
--fates PATH     指定 fates.exe 路径
--no-browser     启动后不打开浏览器
--verbose        输出 HTTP 访问日志
```

服务器会在终端显示实际访问地址。将 `fates.exe` 和静态资源放在可访问的位置即可；若使用独立程序，把 `fates-web.exe` 放在 `fates.exe` 旁边。

## 构建 Windows 程序

`build_web.ps1` 会在临时目录安装 PyInstaller，构建完成后删除临时目录，并把结果写入项目根目录：

```powershell
.\frontend\build_web.ps1
```

生成的 `fates-web.exe` 属于构建产物，不应提交到源码仓库；项目根目录的 `.gitignore` 已包含相应规则。
构建依赖固定在 `requirements-build.txt`，由 Dependabot 定期检查更新。

GitHub Actions 的 `Build release artifacts` 工作流也会构建网页程序，并将它与 Windows AVX2 PGO 版 `fates.exe` 放在同一个 artifact 中。
