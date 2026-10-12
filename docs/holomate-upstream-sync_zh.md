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

建议在网络正常时补全历史，以便后续合并和冲突分析：

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
git stash push -m "work before upstream sync"
```

获取官方更新，并合并到 HoloMate 分支。该分支已发布到个人 Fork，
使用 merge 保留双方历史，后续推送不需要重写远程提交：

```powershell
git fetch upstream
git merge --no-ff upstream/main
```

同步完成后建议编译验证：

```powershell
& 'C:\Users\fh\.codex\skills\build-flash-holomate\scripts\Build-Flash-HoloMate-Waveshare.ps1'
```

如果同步前使用了 stash，可在合并完成后恢复。使用 apply 保留备份，
确认恢复和编译无误后再删除对应的 stash：

```powershell
git stash apply
```

## 处理合并冲突

发生冲突时先查看文件：

```powershell
git status
```

解决冲突后，将文件标记为已处理并继续：

```powershell
git add <已解决的文件>
git commit
```

如果本次同步不应继续，可以完整取消并回到同步前：

```powershell
git merge --abort
```

HoloMate 最可能出现冲突的位置：

- `main/CMakeLists.txt`：用 `BOARD_DIR` 映射原 HoloMate 和微雪适配板的源码目录。
- `main/Kconfig.projbuild`：HoloMate 板型选项和设备端 AEC 依赖。
- `main/boards/holoMate/`：HoloMate GPIO、音频 Codec 和板级初始化。
- `sdkconfig.holomate.defaults`：HoloMate 板型选择及 `CONFIG_USE_DEVICE_AEC=y`。
- `main/audio/engines/afe_audio_engine.*`：保留 MMR、AEC NORMAL 和 AGC 配置，接入官方内存与模型修复。
- `main/application.*` / `main/protocols/`：保留异常断线恢复，合入协议校验和队列修复。
- `scripts/build_holomate_waveshare.py`：调用官方 `scripts/build.py` 的版本检测。

微雪适配板的 `config.json` 固定 `type=holomate-waveshare-s3-lcd-1.3`，
不要让目录前缀改变上报的 OTA 板型。专用构建脚本中的 `LOCAL_PROFILE`
保留本地默认唤醒模型、Flash 布局及官方 SRAM/PSRAM 内存配置。

处理冲突时，应保留官方对公共框架的更新，再把 HoloMate 所需的最小配置接入新版结构，
不要直接用旧文件覆盖官方新文件。

## 配置个人 Fork

先在 GitHub 上 Fork `78/xiaozhi-esp32`，然后将自己的仓库添加为 `origin`：

```powershell
git remote add origin https://github.com/<你的用户名>/xiaozhi-esp32.git
git push -u origin holomate
```

合并并验证后，使用普通推送：

```powershell
git push origin holomate
```

本流程保留已有历史，无需强制推送。

## 推荐的完整同步流程

```powershell
git switch holomate
git status
git fetch upstream
git merge --no-ff upstream/main
& 'C:\Users\fh\.codex\skills\build-flash-holomate\scripts\Build-Flash-HoloMate-Waveshare.ps1'
git push origin holomate
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
- `captures/` 音频抓取文件（已加入 `.gitignore`）。

## 2026-10-12 同步基线

本次合入官方 `bb1c094`（v2.5.2 发布后的 main），保留 HoloMate 的
ESP-SR 2.5.5、双麦 MMR、36 dB 增益、AEC NORMAL、AGC 和镜像字幕。
本机继续使用官方支持的 ESP-IDF 6.0.2；官方最低要求为 6.0.1。

主机回归测试用 ESP-IDF Python 的 UTF-8 模式运行；Windows 下将已有的
Git Bash 加入 PATH，以运行 CI 路径筛选测试。缺少主机 C++ 编译器时，
对应的 Ogg/ESP-VoCat 主机测试会明确跳过；固件编译和物理音频测试是独立验证。

正常代码烧录继续使用 `app-flash`，保留设备上的自定义 assets；只有资源、
模型或分区布局变化且确实需要更新时，才单独评估完整烧录。

板级功能应尽量限制在 `main/boards/holoMate/` 和专用默认配置中；只有官方构建系统需要
识别 HoloMate 时，才修改公共的 `main/CMakeLists.txt` 或 `main/Kconfig.projbuild`。
