# HoloMate 跟随小智官方仓库更新

本文档用于维护 HoloMate 板级适配，同时持续同步小智官方仓库：

- 官方仓库：`https://github.com/78/xiaozhi-esp32.git`
- 官方远程名称：`upstream`
- HoloMate 开发分支：`holomate`
- 个人 Fork 远程名称：`origin`（可选）

## 分支和远程约定

不要直接在官方 `main` 分支上开发 HoloMate。所有板级修改都应提交到
`holomate` 分支，官方更新则从 `upstream/main` 获取。

查看当前状态：

```powershell
git branch --show-current
git remote -v
git status
```

正常情况下应看到当前分支为 `holomate`，并且 `upstream` 指向官方仓库。

## 首次补全浅克隆历史

如果执行下面的命令返回 `true`，说明仓库是浅克隆：

```powershell
git rev-parse --is-shallow-repository
```

建议在网络正常时补全历史，以便后续 rebase 和冲突分析：

```powershell
git fetch upstream --unshallow
```

如果暂时不希望下载完整历史，可以逐步增加历史深度：

```powershell
git fetch upstream --deepen=500
```

## 日常同步官方代码

同步前先确认工作区干净：

```powershell
git switch holomate
git status
```

如果有尚未完成的修改，应先提交；临时修改也可以先保存：

```powershell
git stash push -u -m "work before upstream sync"
```

获取官方更新，并将 HoloMate 提交重新应用到最新版官方代码之上：

```powershell
git fetch upstream
git rebase upstream/main
```

同步完成后建议编译验证：

```powershell
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.holomate.defaults" build
```

如果同步前使用了 stash，可在 rebase 完成后恢复：

```powershell
git stash pop
```

## 处理 rebase 冲突

发生冲突时先查看文件：

```powershell
git status
```

解决冲突后，将文件标记为已处理并继续：

```powershell
git add <已解决的文件>
git rebase --continue
```

如果本次同步不应继续，可以完整取消并回到同步前：

```powershell
git rebase --abort
```

HoloMate 最可能出现冲突的位置：

- `main/CMakeLists.txt`：HoloMate 板型到 `main/boards/holoMate` 的映射。
- `main/Kconfig.projbuild`：HoloMate 板型选项和设备端 AEC 依赖。
- `main/boards/holoMate/`：HoloMate GPIO、音频 Codec 和板级初始化。
- `sdkconfig.holomate.defaults`：HoloMate 板型选择及 `CONFIG_USE_DEVICE_AEC=y`。

处理冲突时，应保留官方对公共框架的更新，再把 HoloMate 所需的最小配置接入新版结构，
不要直接用旧文件覆盖官方新文件。

## 配置个人 Fork

先在 GitHub 上 Fork `78/xiaozhi-esp32`，然后将自己的仓库添加为 `origin`：

```powershell
git remote add origin https://github.com/<你的用户名>/xiaozhi-esp32.git
git push -u origin holomate
```

因为 rebase 会重写 `holomate` 分支提交，后续同步官方后应使用：

```powershell
git push --force-with-lease origin holomate
```

不要使用普通的 `--force`，`--force-with-lease` 会在远程分支出现未知新提交时拒绝覆盖。

## 推荐的完整同步流程

```powershell
git switch holomate
git status
git fetch upstream
git rebase upstream/main
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.holomate.defaults" build
git push --force-with-lease origin holomate
```

如果没有配置个人 Fork，则省略最后一条 `git push`。

## 提交范围注意事项

提交前使用以下命令审查实际改动：

```powershell
git status --short
git diff --check
git diff
```

不要提交以下本地生成内容：

- `build/`、`build-holomate-*` 等编译目录。
- 项目根目录的 `sdkconfig`。
- 串口日志、临时脚本和本地 Python 虚拟环境。

板级功能应尽量限制在 `main/boards/holoMate/` 和专用默认配置中；只有官方构建系统需要
识别 HoloMate 时，才修改公共的 `main/CMakeLists.txt` 或 `main/Kconfig.projbuild`。
