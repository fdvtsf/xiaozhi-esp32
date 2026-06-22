# ============================================================================
# 清理 CUDA 10.1 环境变量脚本
# 功能: 从系统 PATH 中移除 CUDA 10.1 相关路径,仅保留 CUDA 12.4
# 使用方法: 以管理员身份运行 PowerShell,然后执行此脚本
# ============================================================================

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  CUDA 10.1 环境变量清理工具" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# 检查是否以管理员权限运行
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "错误: 此脚本需要管理员权限才能修改系统环境变量!" -ForegroundColor Red
    Write-Host "请右键点击 PowerShell,选择 '以管理员身份运行',然后重新执行此脚本。" -ForegroundColor Yellow
    exit 1
}

Write-Host "[1/4] 检查当前系统 PATH 中的 CUDA 路径..." -ForegroundColor Green
$currentPath = [System.Environment]::GetEnvironmentVariable('Path', 'Machine')
$cudaPaths = $currentPath -split ';' | Where-Object { $_ -match 'CUDA' }

if ($cudaPaths.Count -eq 0) {
    Write-Host "未找到任何 CUDA 相关路径。" -ForegroundColor Yellow
    exit 0
}

Write-Host "当前 CUDA 路径:" -ForegroundColor Cyan
$cudaPaths | ForEach-Object { Write-Host "  - $_" -ForegroundColor White }
Write-Host ""

# 识别需要删除的 CUDA 10.1 路径
$pathsToRemove = $cudaPaths | Where-Object { $_ -match 'CUDA\\v10\.1' }

if ($pathsToRemove.Count -eq 0) {
    Write-Host "✓ 未发现 CUDA 10.1 路径,无需清理。" -ForegroundColor Green
    exit 0
}

Write-Host "[2/4] 发现以下 CUDA 10.1 路径需要删除:" -ForegroundColor Yellow
$pathsToRemove | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
Write-Host ""

# 确认操作
Write-Host "即将从系统 PATH 中删除上述 CUDA 10.1 路径。" -ForegroundColor Yellow
$confirm = Read-Host "是否继续? (输入 Y 确认,其他键取消)"

if ($confirm -ne 'Y' -and $confirm -ne 'y') {
    Write-Host "操作已取消。" -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "[3/4] 正在清理 CUDA 10.1 路径..." -ForegroundColor Green

# 构建新的 PATH (移除所有包含 CUDA\v10.1 的路径)
$newPath = ($currentPath -split ';' | Where-Object { $_ -notmatch 'CUDA\\v10\.1' }) -join ';'

# 更新系统环境变量
try {
    [System.Environment]::SetEnvironmentVariable('Path', $newPath, 'Machine')
    Write-Host "✓ 系统 PATH 已成功更新!" -ForegroundColor Green
} catch {
    Write-Host "✗ 更新系统 PATH 失败: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[4/4] 验证清理结果..." -ForegroundColor Green

# 重新读取并显示更新后的 CUDA 路径
$updatedPath = [System.Environment]::GetEnvironmentVariable('Path', 'Machine')
$remainingCudaPaths = $updatedPath -split ';' | Where-Object { $_ -match 'CUDA' }

if ($remainingCudaPaths.Count -eq 0) {
    Write-Host "警告: 系统中已无任何 CUDA 路径!" -ForegroundColor Red
    Write-Host "可能需要重新配置 CUDA 12.4 的环境变量。" -ForegroundColor Yellow
} else {
    Write-Host "剩余 CUDA 路径:" -ForegroundColor Cyan
    $remainingCudaPaths | ForEach-Object { Write-Host "  - $_" -ForegroundColor Green }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  清理完成!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "重要提示:" -ForegroundColor Yellow
Write-Host "1. 请关闭所有已打开的终端窗口,然后重新打开以使更改生效" -ForegroundColor White
Write-Host "2. 验证命令: `$env:Path -split ';' | Select-String -Pattern 'CUDA'" -ForegroundColor White
Write-Host "3. 如果仍有问题,可能需要重启计算机" -ForegroundColor White
Write-Host ""


