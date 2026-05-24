# Helper script to build, run, and test b1nix via Arch Linux WSL
# Run with: PowerShell -ExecutionPolicy Bypass -File .\run-wsl.ps1

Write-Host "===============================================" -ForegroundColor Green
Write-Host "  Build and run b1nix in Arch Linux WSL (Win)   " -ForegroundColor Green
Write-Host "===============================================" -ForegroundColor Green

# 1. Verify WSL exists
$wslCheck = Get-Command wsl -ErrorAction SilentlyContinue
if (-not $wslCheck) {
    Write-Host "[Error] WSL is not installed on your system." -ForegroundColor Red
    Write-Host "To install WSL:" -ForegroundColor Yellow
    Write-Host "1. Open PowerShell as Administrator."
    Write-Host "2. Run: wsl --install"
    Write-Host "3. Reboot your computer and run this script again."
    Read-Host "Press Enter to exit..."
    exit 1
}

# 2. Check WSL distros
$wslDistros = wsl --list --quiet 2>$null
if (-not $wslDistros) {
    Write-Host "[Error] WSL is installed, but no Linux distribution was found." -ForegroundColor Red
    Write-Host "Please ensure Arch Linux is installed." -ForegroundColor Yellow
    Read-Host "Press Enter to exit..."
    exit 1
}

# Distro name
$distro = "archlinux"

# Helper function to check if a tool is present in WSL
function Test-WslTool ($toolName) {
    $check = wsl -d $distro sh -c "command -v $toolName" 2>$null
    return ($LASTEXITCODE -eq 0)
}

# Helper function to install dependencies
function Install-WslDependencies {
    Write-Host "`nInstalling dependencies in Arch Linux (WSL)..." -ForegroundColor Cyan
    Write-Host "Running installation as root inside WSL..." -ForegroundColor Yellow
    wsl -d $distro -u root pacman -Syu --needed --noconfirm lld grub xorriso xxd mtools dosfstools
    if ($LASTEXITCODE -eq 0) {
        Write-Host "All dependencies installed successfully!" -ForegroundColor Green
    } else {
        Write-Host "Error installing dependencies. Please run this command manually in Arch Linux:" -ForegroundColor Red
        Write-Host "  pacman -Syu --needed lld grub xorriso xxd mtools dosfstools" -ForegroundColor Yellow
    }
}

# 3. Interactive menu
while ($true) {
    Write-Host "`nChoose an option:" -ForegroundColor White
    Write-Host "1) Verify / Install dependencies in WSL (pacman)" -ForegroundColor Gray
    Write-Host "2) Build project (make all)" -ForegroundColor Gray
    Write-Host "3) Run in QEMU via WSL (WSLg / GUI required)" -ForegroundColor Gray
    Write-Host "4) Run in QEMU in console mode (Headless / Serial)" -ForegroundColor Gray
    Write-Host "5) Run in QEMU via Windows-native QEMU" -ForegroundColor Gray
    Write-Host "6) Run standard smoke tests (make smoke-x86)" -ForegroundColor Gray
    Write-Host "7) Run graphics smoke tests (make graphics-smoke)" -ForegroundColor Gray
    Write-Host "8) Clean build files (make clean)" -ForegroundColor Gray
    Write-Host "q) Exit" -ForegroundColor Gray
    
    $choice = Read-Host "`nEnter option number"
    
    switch ($choice) {
        "1" {
            $missing = @()
            $tools = @("clang", "ld.lld", "make", "grub-mkrescue", "xorriso", "mke2fs", "xxd")
            foreach ($t in $tools) {
                if (-not (Test-WslTool $t)) {
                    $missing += $t
                }
            }
            
            if ($missing.Count -eq 0) {
                Write-Host "All build tools (clang, lld, make, grub, xorriso, mke2fs, xxd) are found in WSL!" -ForegroundColor Green
            } else {
                Write-Host "Missing tools in WSL: $($missing -join ', ')" -ForegroundColor Yellow
                $confirm = Read-Host "Would you like to install them automatically? (Y/N)"
                if ($confirm -eq 'y' -or $confirm -eq 'Y') {
                    Install-WslDependencies
                }
            }
        }
        
        "2" {
            Write-Host "`nBuilding project..." -ForegroundColor Cyan
            wsl -d $distro make all
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Build completed successfully!" -ForegroundColor Green
            } else {
                Write-Host "Build failed." -ForegroundColor Red
            }
        }
        
        "3" {
            Write-Host "`nRunning b1nix in QEMU via WSL (with graphical output)..." -ForegroundColor Cyan
            wsl -d $distro make run-x86
        }
        
        "4" {
            Write-Host "`nRunning b1nix in console mode (Headless, output to terminal)..." -ForegroundColor Cyan
            if (-not (Test-Path "build/x86/b1nix.iso") -or -not (Test-Path "build/x86/root.ext4")) {
                Write-Host "Images not found. Building first..." -ForegroundColor Yellow
                wsl -d $distro make iso userspace-install root-image
            }
            if ($LASTEXITCODE -eq 0) {
                wsl -d $distro sh -c "cp build/x86/b1nix.iso /tmp/b1nix-run.iso && qemu-system-x86_64 -cdrom /tmp/b1nix-run.iso -serial stdio -display none -monitor none -no-reboot -boot d -drive file=build/x86/root.ext4,format=raw,if=virtio -netdev user,id=n0 -device virtio-net-pci,netdev=n0"
            }
        }
        
        "5" {
            Write-Host "`nRunning b1nix via Windows-native QEMU..." -ForegroundColor Cyan
            $qemuWin = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
            if (-not $qemuWin) {
                Write-Host "[Error] qemu-system-x86_64 was not found in Windows PATH." -ForegroundColor Red
                Write-Host "You can install it using winget:" -ForegroundColor Yellow
                Write-Host "  winget install QEMU.QEMU" -ForegroundColor White
                Write-Host "Or download it from: https://www.qemu.org/download/#windows"
            } else {
                if (-not (Test-Path "build/x86/b1nix.iso") -or -not (Test-Path "build/x86/root.ext4")) {
                    Write-Host "Images not found. Building in WSL first..." -ForegroundColor Yellow
                    wsl -d $distro make iso userspace-install root-image
                }
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "Launching QEMU..." -ForegroundColor Green
                    qemu-system-x86_64.exe -cdrom build/x86/b1nix.iso -serial stdio -no-reboot -boot d -drive file=build/x86/root.ext4,format=raw,if=virtio -netdev user,id=n0 -device virtio-net-pci,netdev=n0
                }
            }
        }
        
        "6" {
            Write-Host "`nRunning standard smoke tests..." -ForegroundColor Cyan
            wsl -d $distro make smoke-x86
        }
        
        "7" {
            Write-Host "`nRunning graphics smoke tests..." -ForegroundColor Cyan
            wsl -d $distro make graphics-smoke
        }
        
        "8" {
            Write-Host "`nCleaning build files..." -ForegroundColor Cyan
            wsl -d $distro make clean
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Cleaned!" -ForegroundColor Green
            }
        }
        
        "q" {
            break
        }
        
        default {
            Write-Host "Invalid choice. Please enter 1-8 or q." -ForegroundColor Yellow
        }
    }
}
