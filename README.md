# JdkManagement

中文：
`JdkManagement` 是一个面向 Windows 的多运行时管理工具原型，使用 C++ 实现，目标是为 Java、Python、Node.js、Go、Maven、Gradle 提供统一的安装、切换、查询、诊断与项目级环境管理体验。

English:
`JdkManagement` is a Windows-first multi-runtime manager prototype written in C++. It provides a unified workflow for installing, selecting, querying, diagnosing, and activating Java, Python, Node.js, Go, Maven, and Gradle runtimes.

Developer: `Aenlly`

## 文档维护 / Documentation Maintenance

中文：
- 本 README 是面向使用者与实现者的主说明文档。
- 新增、修改、删除功能时，必须在同一批改动中同步更新本 README。
- 下列变化都应更新文档：命令、参数、默认行为、支持来源、输出示例、状态说明、目录结构、安装方式、限制条件。
- 如果某项能力仍处于实验阶段，应在中英文两部分同时明确标注。
- 文档只描述项目当前已实现或已确认的行为，避免夸大、避免超出实现范围的表述。
- 对第三方来源仅保留必要的技术集成描述，避免使用容易引发授权或商务误解的措辞。

English:
- This README is the primary document for both users and implementers.
- Any feature addition, change, or removal must update this README in the same change set.
- The README must be updated when commands, flags, defaults, supported sources, output examples, status semantics, directory layout, installation steps, or limitations change.
- Experimental behavior should be marked clearly in both Chinese and English.
- The document should describe only implemented or confirmed behavior.
- Keep third-party references strictly technical and integration-oriented, and avoid wording that may create licensing or commercial ambiguity.

## 项目目标 / Project Goals

中文：
- 统一管理常见开发运行时与工具链。
- 同时支持用户级全局选择与项目级本地覆盖。
- 提供可追踪、可恢复、可诊断的安装体验。
- 让下载、校验、解压、激活这些步骤具有一致的日志与交互风格。

English:
- Unify management of common development runtimes and toolchains.
- Support both user-scoped global selections and project-scoped local overrides.
- Provide an install workflow that is traceable, recoverable, and diagnosable.
- Keep download, verification, extraction, and activation stages consistent in logging and interaction style.

## 当前能力 / Current Capabilities

中文：
- Java、Python、Node.js、Go、Maven、Gradle 的安装、卸载、切换、查询。
- 用户级与项目级运行时选择。
- PowerShell / pwsh Shell Hook 与会话激活支持。
- Python 环境创建、删除、激活。
- 远程版本查询与表格/JSON 输出。
- 基础诊断、操作日志与最近记录查看。
- 分阶段安装日志：解析、下载、校验、解压、完成。
- 下载进度平滑显示：平滑速率、平滑 ETA、早期 ETA 门控。
- 下载恢复能力：自动重试、`.partial` 续传、完成后原子落盘。

English:
- Install, remove, switch, and query Java, Python, Node.js, Go, Maven, and Gradle runtimes.
- Support user-scoped and project-scoped runtime selection.
- Provide PowerShell / pwsh shell hook integration and session activation.
- Support Python environment creation, removal, and activation.
- Support remote version listing with table and JSON output.
- Provide basic diagnostics, operational logs, and recent-log inspection.
- Emit staged install logs for resolve, download, verification, extraction, and completion.
- Provide smoothed download progress with stabilized rate, stabilized ETA, and early ETA gating.
- Support download recovery with retries, `.partial` resume, and atomic finalization.

## 默认支持来源 / Default Upstream Sources

中文：
- Java: `Temurin`
- Python: `CPython` 包源与本地 `venv` 环境
- Node.js: `nodejs.org` 压缩包来源
- Go: `go.dev` 压缩包来源
- Maven: Apache Maven 发行归档
- Gradle: Gradle 发行服务

English:
- Java: `Temurin`
- Python: `CPython` package source and local `venv` environments
- Node.js: `nodejs.org` archive downloads
- Go: `go.dev` archive downloads
- Maven: Apache Maven distribution archive
- Gradle: Gradle distribution service

## 命令概览 / Command Overview

说明 / Notes:
- `<type>` 支持 `java|python|node|go|maven|gradle`
- `<selector>` 可以是大版本、精确版本或项目支持的其他选择器形式

| Command | 中文说明 | English |
| --- | --- | --- |
| `jkm init` | 初始化用户环境与运行时目录 | Initialize user environment and runtime directories |
| `jkm deinit` | 恢复原始环境配置 | Restore the original environment configuration |
| `jkm install <type> <selector>` | 安装指定运行时 | Install a runtime |
| `jkm remove <type> <selector>` | 删除已管理的运行时 | Remove a managed runtime |
| `jkm uninstall <type> <selector>` | `remove` 的别名形式 | Alias of `remove` |
| `jkm search <type> [...]` | 查询远程版本 | Search remote versions |
| `jkm remote list <type> [...]` | 列出远程候选版本 | List remote candidates |
| `jkm version <type> [selector]` | 查看版本信息 | Show version information |
| `jkm status [type]` | 查看本地/全局/生效状态 | Show local/global/effective status |
| `jkm list [type]` | 列出已安装运行时 | List installed runtimes |
| `jkm current [type]` | 查看当前生效运行时 | Show the current effective runtime |
| `jkm use <type> <selector>` | 设为当前使用版本 | Set the active runtime |
| `jkm unuse <type>` | 清除当前作用域下的选择 | Clear the active selection in the current scope |
| `jkm exec <command> [args...]` | 在生效环境中执行命令 | Run a command in the effective runtime environment |
| `jkm shell hook <powershell\|pwsh>` | 输出 Shell Hook | Print shell hook content |
| `jkm shell install <powershell\|pwsh>` | 安装 Shell Hook | Install the shell hook |
| `jkm shell uninstall <powershell\|pwsh>` | 卸载 Shell Hook | Remove the shell hook |
| `jkm shell status` | 查看 Hook 状态 | Check shell hook status |
| `jkm doctor` | 运行基础诊断 | Run basic diagnostics |
| `jkm logs path` | 输出日志目录 | Print the log directory |
| `jkm logs recent [...]` | 查看最近操作记录 | Show recent operation records |
| `jkm logs show --operation <id>` | 查看单次操作详情 | Show one operation in detail |
| `jkm env list python` | 列出 Python 环境 | List Python environments |
| `jkm env create python <name>` | 创建 Python 环境 | Create a Python environment |
| `jkm env remove python <name>` | 删除 Python 环境 | Remove a Python environment |
| `jkm env activate` | 生成当前目录激活脚本 | Generate an activation script for the current directory |

## 使用示例 / Usage Examples

```powershell
jkm init
jkm install java 21
jkm use java 21
jkm install node 22 --arch x64
jkm use node 22 --local
jkm exec node --version
jkm search python 3.12 --format json
jkm remote list maven 3.9 --latest
jkm env create python dev --python 3.12
jkm env activate --shell powershell | Out-String | Invoke-Expression
jkm logs recent --limit 10
```

## 作用域模型 / Scope Model

中文：
- 默认 `jkm use ...` 写入用户级选择。
- `--scope local` 或 `--local` 会写入项目目录下的 `.jkm\local_runtimes.tsv`。
- `jkm current` 与 `jkm version` 会优先读取当前目录向上最近的项目级覆盖，再回退到用户级选择。
- `jkm exec ...` 会按“项目级优先、用户级回退”的规则构造执行环境。
- `jkm unuse ... --local` 只移除当前项目覆盖，不影响用户级配置。

English:
- `jkm use ...` writes a user-scoped selection by default.
- `--scope local` or `--local` writes a project-local override into `.jkm\local_runtimes.tsv`.
- `jkm current` and `jkm version` resolve the nearest project override first, then fall back to the user-scoped selection.
- `jkm exec ...` runs commands with the effective environment built from local-over-user resolution.
- `jkm unuse ... --local` removes only the current project override.

## 下载与安装行为 / Download and Install Behavior

中文：
- 下载日志会按阶段输出，便于定位问题。
- 下载进度默认显示文件大小、进度百分比、平滑速率，以及在条件满足后显示 ETA。
- 下载中断后，如存在 `.partial` 文件，会尝试自动续传。
- 下载失败会自动重试，并输出重试原因与等待时间。
- 当上游提供校验值时，安装前会执行校验。
- 安装成功前，归档文件会先保留在缓存目录中，完成后再写入最终状态。

English:
- Download logs are emitted by stage to simplify troubleshooting.
- Progress output shows file size, percentage, stabilized transfer rate, and ETA once the signal is considered reliable.
- If a `.partial` file exists, the downloader attempts to resume from it.
- Failed downloads are retried automatically with reason and delay logging.
- Checksums are verified when upstream metadata provides them.
- Archives remain in the cache area during install and are finalized only after download completion.

## 原始环境处理 / Original Environment Handling

中文：
- 第一次执行 `init` 或首次建立用户级选择时，会保存原始 PATH 与相关运行时变量快照。
- 已检测到的外部运行时可以以 `original` 形式保留为只读入口。
- `jkm deinit` 与完整卸载会尽量恢复原始用户环境。

English:
- The first `init` or first user-scoped selection captures a snapshot of the original PATH and related runtime variables.
- Previously detected external runtimes may be preserved as a read-only `original` entry.
- `jkm deinit` and full uninstall attempt to restore the original user environment.

## 构建与安装 / Build and Install

中文：
- Windows 构建命令：

```powershell
.\build.ps1
```

- 产物路径：`build\jkm.exe`
- 当前用户安装：

```powershell
.\scripts\install-jkm.ps1
```

- 默认路径：
  - 可执行文件：`%LOCALAPPDATA%\Programs\JdkManagement\bin\jkm.exe`
  - 数据目录：`%LOCALAPPDATA%\JdkManagement`
- 如果设置 `JDKM_HOME`，程序会优先使用该目录作为数据根目录。

English:
- Build on Windows with:

```powershell
.\build.ps1
```

- Output binary: `build\jkm.exe`
- Install for the current user with:

```powershell
.\scripts\install-jkm.ps1
```

- Default paths:
  - Executable: `%LOCALAPPDATA%\Programs\JdkManagement\bin\jkm.exe`
  - Data root: `%LOCALAPPDATA%\JdkManagement`
- If `JDKM_HOME` is set, it overrides the default data root.

## 目录与数据 / Directory and Data Layout

中文：
- `cache/downloads`: 归档缓存与 `.partial` 续传文件
- `cache/temp`: 解压与中间临时目录
- `installs`: 已安装运行时
- `current`: 当前生效链接
- `logs`: 操作日志
- `state`: 状态与选择记录

English:
- `cache/downloads`: archive cache and `.partial` resume files
- `cache/temp`: extraction and temporary working directories
- `installs`: installed runtimes
- `current`: current effective links
- `logs`: operation logs
- `state`: state and selection records

## 实现约束 / Implementation Rules

中文：
- 功能实现完成后，如果用户可见行为发生变化，必须同步更新本 README。
- 新命令、新参数、新输出格式、新下载行为、新缓存策略都不能只改代码不改文档。
- 示例命令应优先保持可复制、可运行、可验证。

English:
- After implementation, any user-visible behavior change must be reflected in this README.
- New commands, flags, output formats, download behavior, and cache strategy changes must update the documentation together with the code.
- Usage examples should remain copyable, runnable, and verifiable.

## 后续方向 / Near-Term Roadmap

中文：
- 统一镜像源、代理与证书配置
- 缓存管理命令：查看、清理、裁剪
- 项目锁定文件与一键同步

English:
- Unified mirror, proxy, and certificate configuration
- Cache management commands for listing and pruning
- Project lock files and one-step sync
