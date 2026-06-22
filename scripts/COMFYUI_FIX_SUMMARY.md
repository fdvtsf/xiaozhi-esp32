# ComfyUI 问题修复总结

## 🎉 问题已解决!

### 原始问题
ComfyUI 无法启动,CUDA 不可用,报错 `cudaGetDeviceCount() returned cudaErrorNotSupported`

### 根本原因
- **驱动版本过旧**: 555.97 (仅支持 CUDA 12.5)
- **PyTorch 版本需求**: ComfyUI 内置 PyTorch 2.11.0+cu130 需要 CUDA 13.0
- **版本不匹配**: 驱动不支持 PyTorch 所需的 CUDA 版本

---

## ✅ 解决方案

### 步骤 1: 更新 NVIDIA 驱动
- **原版本**: 555.97
- **新版本**: 596.36 (支持 CUDA 13.2)
- **更新方式**: 使用 NVIDIA App 更新 Studio 驱动

### 步骤 2: 清理环境变量(可选但推荐)
- 移除了系统中旧的 CUDA 10.1 路径
- 保留 CUDA 12.4 路径用于其他项目

### 步骤 3: 验证 CUDA 可用性
```powershell
# 检查驱动版本
nvidia-smi

# 检查 ComfyUI CUDA 状态
cd G:\0ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable
.\python_embeded\python.exe -c "import torch; print('CUDA:', torch.cuda.is_available())"
```

---

## 📊 当前配置

| 组件 | 版本/状态 |
|------|----------|
| NVIDIA 驱动 | 596.36 |
| CUDA 支持 | 13.2 |
| GPU | GTX 1650 (4GB VRAM) |
| PyTorch | 2.11.0+cu130 |
| CUDA 可用 | ✅ True |
| ComfyUI | 0.21.0 |

---

## 🚀 启动 ComfyUI

### 方法 1: 使用批处理文件
```batch
G:\0ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable\run_nvidia_gpu.bat
```

### 方法 2: 手动启动
```powershell
cd G:\0ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable
.\python_embeded\python.exe -s ComfyUI\main.py --windows-standalone-build
```

### 访问地址
- **本地**: http://127.0.0.1:8188
- **局域网**: http://0.0.0.0:8188

---

## 🔧 额外修复

### 安装 OpenCV (ControlNet 支持)
```powershell
cd G:\0ComfyUI\ComfyUI_windows_portable_nvidia\ComfyUI_windows_portable
.\python_embeded\python.exe -m pip install opencv-python
```

---

## 💡 经验教训

1. **驱动优先原则**: GPU 加速应用失败时,首先检查并更新显卡驱动
2. **版本兼容性**: 
   - 驱动支持的 CUDA 版本 ≥ PyTorch 编译的 CUDA 版本
   - 使用 `nvidia-smi` 查看驱动支持的 CUDA 版本
   - 使用 `torch.version.cuda` 查看 PyTorch 需要的 CUDA 版本
3. **嵌入式环境**: ComfyUI 等工具使用嵌入式 Python,其 PyTorch 版本独立于系统环境
4. **环境变量清理**: 多个 CUDA 版本路径共存可能导致冲突

---

## 📝 相关脚本

项目中已创建以下辅助脚本:

1. **cleanup_cuda_10.1.ps1** - 清理旧版 CUDA 环境变量
2. **update_nvidia_driver.ps1** - NVIDIA 驱动更新助手
3. **fix_comfyui_pytorch.ps1** - ComfyUI PyTorch 版本修复(备用方案)

位置: `g:\xiaozhiAI\xiaozhi-2.4\scripts\`

---

## ⚠️ 注意事项

1. **数据库锁定**: 如果提示数据库被锁定,确保没有其他 ComfyUI 进程在运行
2. **端口占用**: 默认端口 8188,如被占用可添加 `--port 8189` 参数
3. **显存限制**: GTX 1650 只有 4GB 显存,生成大尺寸图片时可能需要注意内存管理
4. **重启生效**: 驱动更新后必须重启计算机

---

**最后更新**: 2026-05-19
**状态**: ✅ 完全正常工作
