# JdkManagement

[中文说明](README.zh-CN.md)

JdkManagement is a Windows-first runtime manager prototype written in C++. It provides one CLI for installing, selecting, querying, and activating Java, Python, Node.js, Go, Maven, and Gradle runtimes.

## Documentation Maintenance

- `README.md` and `README.zh-CN.md` are required project documents.
- Any user-visible feature addition, change, or removal must update both files in the same change set.
- Keep command examples runnable and aligned with current behavior.
- Document only implemented or verified behavior. Mark experiments clearly.
- Keep third-party references technical and integration-focused.

## Current Scope

- Install, remove, list, query, and switch Java, Python, Node.js, Go, Maven, and Gradle runtimes.
- Support user-scoped selections and project-local overrides in `.jkm/local_runtimes.tsv`.
- Support Python environment list, create, remove, and activate flows.
- Provide remote version listing with table and JSON output.
- Emit staged install logs with smoothed download rate and ETA.
- Resume interrupted downloads from `.partial` files and retry failed transfers automatically.
- Persist mirror, proxy, and certificate settings through `jkm config`.
- Manage cached download and temp files through `jkm cache`.
- Capture project runtime selections into a lock file and replay them through `jkm sync`.

## Command Summary

### Core runtime commands

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

`<type>` supports `java`, `python`, `node`, `go`, `maven`, and `gradle`.

### Python environment commands

```powershell
jkm env list python [--python <base-selector>]
jkm env create python <env-name> [--python <base-selector>]
jkm env remove python <env-selector> [--python <base-selector>]
jkm env activate [--shell powershell|cmd]
```

### Configuration commands

```powershell
jkm config path
jkm config list
jkm config get <key>
jkm config set <key> <value>
jkm config unset <key>
```

### Cache commands

```powershell
jkm cache list [downloads|temp|all]
jkm cache clear [downloads|temp|all]
jkm cache prune [downloads|temp|all] [--max-size-mb <n>] [--max-age-days <n>] [--dry-run]
```

### Project lock commands

```powershell
jkm lock path [--lock-file <path>]
jkm lock show [--lock-file <path>]
jkm lock write [--lock-file <path>]
jkm sync [--scope local|user] [--lock-file <path>]
```

## Persisted Mirror, Proxy, and Certificate Settings

Settings are stored in `state/settings.tsv` and are applied when the CLI starts.

| Key | Purpose |
| --- | --- |
| `network.proxy` | Shared proxy applied to both HTTP and HTTPS unless overridden per protocol |
| `network.http_proxy` | HTTP proxy override |
| `network.https_proxy` | HTTPS proxy override |
| `network.ca_cert` | Path to a PEM or certificate bundle used as an extra trusted root set |
| `mirror.temurin` | Base URL for Temurin API and package requests |
| `mirror.python` | Base URL for Python package metadata and archives |
| `mirror.node` | Base URL for Node.js metadata and archives |
| `mirror.go` | Base URL for Go metadata and archives |
| `mirror.maven.metadata` | Base URL for Maven metadata requests |
| `mirror.maven.archive` | Base URL for Maven archives |
| `mirror.gradle` | Base URL for Gradle metadata and distributions |

Behavior notes:

- `network.ca_cert` is normalized to an absolute path when persisted.
- Mirror values are stored without a trailing slash.
- The downloader uses persisted `JDKM_*` settings first, then standard process variables such as `HTTP_PROXY`, `HTTPS_PROXY`, and `SSL_CERT_FILE`.
- The same helper layer is shared by all supported runtime providers.

Example:

```powershell
jkm config set network.proxy http://127.0.0.1:7890
jkm config set network.ca_cert .\certs\corp-root.pem
jkm config set mirror.node https://mirror.example.com/node
jkm config list
```

## Cache Management

Cache data lives under `cache/downloads` and `cache/temp`.

- `jkm cache list` prints each cached file with its area and size.
- `jkm cache clear` removes everything under the selected cache area and recreates the root directories.
- `jkm cache prune` removes files by age, total size cap, or both.
- `--dry-run` shows what would be removed without deleting files.

Example:

```powershell
jkm cache list
jkm cache prune downloads --max-age-days 30 --dry-run
jkm cache prune all --max-size-mb 1024
jkm cache clear temp
```

## Project Lock and Sync

The default project lock file is `.jkm/project.lock.tsv`.

- `jkm lock write` captures the current managed runtime selections for the project view.
- Preserved external selections such as `original` are skipped instead of being written into the lock file.
- `jkm lock show` prints the lock metadata and runtime entries.
- `jkm sync` replays the lock file by installing the requested runtimes and then applying `jkm use`.
- `jkm sync` defaults to `--scope local`.
- Local sync resolves the project root from the lock file path before writing `.jkm/local_runtimes.tsv`.
- Python environment entries are restored by ensuring the base interpreter is installed first, then creating the named environment if needed.

Example:

```powershell
jkm use java 21 --local
jkm use node 22 --local
jkm lock write
jkm lock show
jkm sync
```

## Download Behavior

Download behavior is centralized in a shared helper used by all runtime providers.

- Progress logs are emitted by stage.
- Transfer rate is smoothed to avoid unstable early samples.
- ETA display is gated until enough progress or elapsed time exists.
- Failed downloads retry automatically with backoff.
- Partial files are resumed when the upstream server supports range requests.
- Final files are moved into place only after a completed transfer.

## Build and Install

Build on Windows:

```powershell
.\build.ps1
```

Default output:

- `build\jkm.exe`

Install for the current user:

```powershell
.\scripts\install-jkm.ps1
```

Default paths:

- Executable: `%LOCALAPPDATA%\Programs\JdkManagement\bin\jkm.exe`
- Data root: `%LOCALAPPDATA%\JdkManagement`
- Override data root with `JDKM_HOME`

## Directory Layout

- `cache/downloads`: archive cache and `.partial` resume files
- `cache/temp`: temporary extraction and working directories
- `installs`: managed runtime installs
- `current`: current runtime links
- `logs`: operation logs
- `state/active_runtimes.tsv`: user-scoped active runtime selections
- `state/settings.tsv`: persisted mirror, proxy, and certificate settings
- `.jkm/local_runtimes.tsv`: project-local runtime selections
- `.jkm/project.lock.tsv`: project lock file

## Usage Examples

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
