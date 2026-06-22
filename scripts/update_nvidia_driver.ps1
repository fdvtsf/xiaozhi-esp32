# NVIDIA Driver Update Helper Script
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  NVIDIA Driver Update Assistant" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Get current GPU info
Write-Host "[1/3] Checking current GPU info..." -ForegroundColor Green
$gpuInfo = nvidia-smi --query-gpu=driver_version,gpu_name --format=csv,noheader,nounits
$gpuData = $gpuInfo -split ',' | ForEach-Object { $_.Trim() }
$currentDriver = $gpuData[0]
$gpuName = $gpuData[1]

Write-Host "GPU Model: $gpuName" -ForegroundColor Cyan
Write-Host "Current Driver: $currentDriver" -ForegroundColor Cyan
Write-Host ""

# Check for GeForce Experience or NVIDIA App
Write-Host "[2/3] Checking NVIDIA management tools..." -ForegroundColor Green
$geforcePath = "$env:LOCALAPPDATA\NVIDIA Corporation\GeForce Experience\NVIDIA GeForce Experience.exe"
$nvidiaAppPath = "$env:ProgramFiles\NVIDIA Corporation\NVIDIA App\NVIDIA App.exe"

if (Test-Path $nvidiaAppPath) {
    Write-Host "Found: NVIDIA App" -ForegroundColor Green
    $toolPath = $nvidiaAppPath
    $toolName = "NVIDIA App"
} elseif (Test-Path $geforcePath) {
    Write-Host "Found: GeForce Experience" -ForegroundColor Green
    $toolPath = $geforcePath
    $toolName = "GeForce Experience"
} else {
    Write-Host "No NVIDIA management tool found" -ForegroundColor Yellow
    $toolPath = $null
}

Write-Host ""

# Provide update options
Write-Host "[3/3] Update Options:" -ForegroundColor Green
Write-Host ""

if ($toolPath) {
    Write-Host "Option A: Use $toolName (Recommended)" -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    $useTool = Read-Host "Open $toolName now? (Y/N)"
    if ($useTool -eq 'Y' -or $useTool -eq 'y') {
        Write-Host "Starting $toolName..." -ForegroundColor Green
        Start-Process $toolPath
        Write-Host ""
        Write-Host "Please complete driver update in $toolName" -ForegroundColor Yellow
        Write-Host "RESTART YOUR COMPUTER AFTER UPDATE!" -ForegroundColor Red
        exit 0
    }
    Write-Host ""
}

Write-Host "Option B: Manual Download from Official Website" -ForegroundColor Cyan
Write-Host "----------------------------------------" -ForegroundColor Gray
$downloadUrl = "https://www.nvidia.cn/Download/index.aspx?lang=cn"
Write-Host "Official Download Page: $downloadUrl" -ForegroundColor Yellow
Write-Host ""

$openBrowser = Read-Host "Open download page in browser? (Y/N)"
if ($openBrowser -eq 'Y' -or $openBrowser -eq 'y') {
    Start-Process $downloadUrl
    Write-Host ""
    Write-Host "Browser opened. Follow these steps:" -ForegroundColor Green
}

Write-Host ""
Write-Host "Manual Download Steps:" -ForegroundColor Cyan
Write-Host "Product Type: GeForce" -ForegroundColor White
Write-Host "Product Series: GeForce 16 Series" -ForegroundColor White
Write-Host "Product Family: GeForce GTX 1650" -ForegroundColor White
Write-Host "OS: Windows 10/11 64-bit" -ForegroundColor White
Write-Host "Download Type: Game Ready Driver (GRD)" -ForegroundColor White
Write-Host "Language: Chinese Simplified" -ForegroundColor White
Write-Host ""
Write-Host "Then click Search -> Download -> Run installer" -ForegroundColor Yellow
Write-Host ""

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Important Notes" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Before updating:" -ForegroundColor Yellow
Write-Host "1. Close all games and graphics applications" -ForegroundColor White
Write-Host "2. Save your work and close unnecessary programs" -ForegroundColor White
Write-Host "3. Ensure laptop is connected to power" -ForegroundColor White
Write-Host "4. Do not shut down during installation" -ForegroundColor White
Write-Host "5. MUST RESTART after installation" -ForegroundColor White
Write-Host ""
Write-Host "Recommended installation options:" -ForegroundColor Yellow
Write-Host "- Choose 'Custom (Advanced)' installation" -ForegroundColor White
Write-Host "- Check 'Perform clean installation' (optional)" -ForegroundColor White
Write-Host ""

Write-Host "After restart, verify with: nvidia-smi" -ForegroundColor Yellow
Write-Host "Look for new driver version (recommend >= 560.x for CUDA 13.0 support)" -ForegroundColor Green
Write-Host ""

Write-Host "Press any key to exit..." -ForegroundColor Gray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
