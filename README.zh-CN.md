# JdkManagement

[English](README.md)

JdkManagement 是一个面向 Windows 的多运行时管理原型，使用 C++ 实现，目标是为 Java、Python、Node.js、Go、Maven 和 Gradle 提供统一的安装、切换、查询与激活体验。

## 文档维护要求

- `README.md` 和 `README.zh-CN.md` 是必须同步维护的主文档。
- 任何用户可见的功能新增、修改或删除，都必须在同一批改动里更新这两个文件。
- 示例命令需要保持可复制、可执行，并与当前行为一致。
- 文档只描述已经实现或已经验证的行为；实验性内容需要明确标注。
- 对第三方来源只保留技术集成层面的说明。

## 当前能力范围

- 安装、卸载、列出、查询和切换 Java、Python、Node.js、Go、Maven、Gradle 运行时。
- 支持用户级选择，以及写入 `.jkm/local_runtimes.tsv` 的项目级覆盖。
- 支持 Python 环境的列表、创建、删除与激活。
- 支持远程版本查询，输出表格或 JSON。
- 提供分阶段安装日志，以及平滑速率和 ETA 的下载进度输出。
- 支持 `.partial` 断点续传和失败自动重试。
- 通过 `jkm config` 统一管理镜像源、代理和证书配置。
- 通过 `jkm cache` 管理下载缓存和临时缓存。
- 通过 `jkm lock` 和 `jkm sync` 固化并回放项目运行时状态。

## 命令概览

### 核心运行时命令

```powershell
jkm init
jkm deinit
jkm install <type> <selector> [--distribution <name>] [--arch x64]
jkm remove <type> <selector>
jkm use <type> <selector> [--scope user|local] [--local]
jkm unuse <type> [--scope user|local] [--local]
jkm status [type]
jkm list [type]
jkm current [type]
jkm version <type> [selector]
jkm search <type> [selector] [--format table|json]
jkm remote list <type> [selector] [--format table|json]
```

`<type>` 支持 `java`、`python`、`node`、`go`、`maven`、`gradle`。

### Python 环境命令

```powershell
jkm env list python [--python <base-selector>]
jkm env create python <env-name> [--python <base-selector>]
jkm env remove python <env-selector> [--python <base-selector>]
jkm env activate [--shell powershell|cmd]
```

### 配置命令

```powershell
jkm config path
jkm config list
jkm config get <key>
jkm config set <key> <value>
jkm config unset <key>
```

### 缓存命令

```powershell
jkm cache list [downloads|temp|all]
jkm cache clear [downloads|temp|all]
jkm cache prune [downloads|temp|all] [--max-size-mb <n>] [--max-age-days <n>] [--dry-run]
```

### 项目锁定命令

```powershell
jkm lock path [--lock-file <path>]
jkm lock show [--lock-file <path>]
jkm lock write [--lock-file <path>]
jkm sync [--scope local|user] [--lock-file <path>]
```

## 统一镜像源、代理与证书配置

配置保存在 `state/settings.tsv`，CLI 启动时会自动加载。

| Key | 作用 |
| --- | --- |
| `network.proxy` | 通用代理；未单独设置时同时作用于 HTTP 和 HTTPS |
| `network.http_proxy` | HTTP 代理覆盖项 |
| `network.https_proxy` | HTTPS 代理覆盖项 |
| `network.ca_cert` | PEM 或证书包路径，用作附加信任根集合 |
| `mirror.temurin` | Temurin API 和安装包基础地址 |
| `mirror.python` | Python 元数据和安装包基础地址 |
| `mirror.node` | Node.js 元数据和安装包基础地址 |
| `mirror.go` | Go 元数据和安装包基础地址 |
| `mirror.maven.metadata` | Maven 元数据基础地址 |
| `mirror.maven.archive` | Maven 安装包基础地址 |
| `mirror.gradle` | Gradle 元数据和分发包基础地址 |

行为说明：

- `network.ca_cert` 持久化时会转换为绝对路径。
- 镜像地址写入时会去掉末尾的 `/`。
- 下载器优先使用持久化后的 `JDKM_*` 配置；如果未设置，再回退到进程中的 `HTTP_PROXY`、`HTTPS_PROXY`、`SSL_CERT_FILE` 等标准环境变量。
- 所有运行时 provider 共用同一层下载与网络辅助逻辑。

示例：

```powershell
jkm config set network.proxy http://127.0.0.1:7890
jkm config set network.ca_cert .\certs\corp-root.pem
jkm config set mirror.node https://mirror.example.com/node
jkm config list
```

## 缓存管理

缓存目录位于 `cache/downloads` 和 `cache/temp`。

- `jkm cache list` 输出每个缓存文件的区域、大小和路径。
- `jkm cache clear` 清空指定区域下的内容，并重建缓存根目录。
- `jkm cache prune` 按最大缓存体积、最大文件年龄或两者组合进行裁剪。
- `--dry-run` 只展示将要删除的文件，不执行删除。

示例：

```powershell
jkm cache list
jkm cache prune downloads --max-age-days 30 --dry-run
jkm cache prune all --max-size-mb 1024
jkm cache clear temp
```

## 项目锁定文件与一键同步

默认锁定文件路径是 `.jkm/project.lock.tsv`。

- `jkm lock write` 会基于当前项目视角下的有效运行时选择写出锁定文件。
- 类似 `original` 这类保留的外部运行时不会写入锁定文件。
- `jkm lock show` 会输出锁定文件的元数据和条目列表。
- `jkm sync` 会按锁定文件安装所需运行时，并执行对应的 `jkm use`。
- `jkm sync` 默认使用 `--scope local`。
- 本地同步时，会以锁定文件所在项目根目录作为 `.jkm/local_runtimes.tsv` 的写入基准。
- Python 环境条目会先确保基础解释器存在，再按需创建命名环境。

示例：

```powershell
jkm use java 21 --local
jkm use node 22 --local
jkm lock write
jkm lock show
jkm sync
```

## 下载行为

下载逻辑集中在统一 helper 中，由所有运行时 provider 共享。

- 下载日志按阶段输出。
- 速率会做平滑处理，减少开头样本抖动。
- ETA 会在进度或耗时达到一定条件后再显示。
- 下载失败会自动退避重试。
- 上游支持范围请求时，会从 `.partial` 文件继续下载。
- 只有下载完整后，文件才会原子落到最终路径。

## 构建与安装

Windows 构建命令：

```powershell
.\build.ps1
```

默认产物：

- `build\jkm.exe`

当前用户安装命令：

```powershell
.\scripts\install-jkm.ps1
```

默认路径：

- 可执行文件：`%LOCALAPPDATA%\Programs\JdkManagement\bin\jkm.exe`
- 数据根目录：`%LOCALAPPDATA%\JdkManagement`
- 如设置 `JDKM_HOME`，则优先使用该目录作为数据根

## 目录结构

- `cache/downloads`：归档缓存和 `.partial` 续传文件
- `cache/temp`：解压和临时工作目录
- `installs`：托管运行时安装目录
- `current`：当前运行时链接
- `logs`：操作日志
- `state/active_runtimes.tsv`：用户级运行时选择
- `state/settings.tsv`：镜像源、代理和证书配置
- `.jkm/local_runtimes.tsv`：项目级运行时选择
- `.jkm/project.lock.tsv`：项目锁定文件

## 使用示例

```powershell
jkm init
jkm install java 21
jkm use java 21
jkm install node 22 --arch x64
jkm use node 22 --local
jkm config set mirror.node https://mirror.example.com/node
jkm cache list
jkm lock write
jkm sync --scope local
```
