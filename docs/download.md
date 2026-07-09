# JdkManagement 下载与安装指南

> 本文面向普通 Windows x64 用户，说明如何从 GitHub Releases 下载、安装、验证和卸载 JdkManagement。

## 1. 下载 Release 包

打开最新 Release 页面：

- [JdkManagement Releases](https://github.com/Aenlly/JdkManagement/releases/latest)

在 **Assets** 区域下载：

```text
JdkManagement-windows-x64.zip
```

解压后应能看到以下文件：

```text
JdkManagement-Setup-windows-x64.exe
JdkManagement-Uninstall-windows-x64.exe
jkm.exe
INSTALL.txt
README.md
README.zh-CN.md
DOWNLOAD.md
docs/download.md
```

## 2. 安装

1. 解压 `JdkManagement-windows-x64.zip`。
2. 在解压目录中运行安装器：

```powershell
.\JdkManagement-Setup-windows-x64.exe
```

3. 重新打开 PowerShell 或 Windows Terminal。
4. 验证安装结果：

```powershell
jkm doctor
```

默认安装路径：

```text
%LOCALAPPDATA%\Programs\JdkManagement
```

默认数据目录：

```text
%LOCALAPPDATA%\JdkManagement
```

安装器会把 `%LOCALAPPDATA%\Programs\JdkManagement\bin` 写入当前用户的 `PATH`。如果当前终端在安装前已经打开，请重新打开终端后再执行 `jkm`。

## 3. PATH 冲突与当前会话激活

如果 `jkm doctor` 报告 nvm、pyenv 或其他工具在 `PATH` 中优先级更高，可以安装 shell hook：

```powershell
jkm shell install powershell
jkm shell install pwsh
```

如果只想临时激活当前 PowerShell 会话，可以执行：

```powershell
jkm env activate --shell powershell | Invoke-Expression
```

## 4. 卸载

可使用以下任一方式卸载。

### 方式 A：Windows 设置

打开 **设置 > 应用 > 已安装的应用**，找到 **JdkManagement** 并选择卸载。

### 方式 B：Release 包中的卸载器

在解压后的 Release 目录执行：

```powershell
.\JdkManagement-Uninstall-windows-x64.exe
```

如果 Release 包已经删除，也可以运行已安装的卸载器：

```powershell
& "$env:LOCALAPPDATA\Programs\JdkManagement\jkm-uninstall.exe"
```

如需同时删除默认数据目录，请追加 `--purge-data`：

```powershell
& "$env:LOCALAPPDATA\Programs\JdkManagement\jkm-uninstall.exe" --purge-data
```

## 5. 升级

1. 从 [Releases](https://github.com/Aenlly/JdkManagement/releases/latest) 下载新的 `JdkManagement-windows-x64.zip`。
2. 解压并重新运行 `JdkManagement-Setup-windows-x64.exe`。
3. 重新打开终端后执行 `jkm doctor` 验证。

升级安装会覆盖程序文件，不会清理 `%LOCALAPPDATA%\JdkManagement` 中的运行时、缓存和配置。

## 6. 从源码构建与打包

开发者可以在仓库根目录运行：

```powershell
.\build.ps1
.\scripts\package-jkm.ps1
```

打包脚本会在 `out\package` 下生成 `JdkManagement-windows-x64.zip` 和展开后的 `JdkManagement` 目录。

源码构建需要 Visual Studio Build Tools、MSVC 和 Windows SDK。普通用户请优先使用 Release 包中的安装器。
