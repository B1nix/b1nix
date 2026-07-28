# Helper script to build, run, and test b1nix via Arch Linux WSL
# Run with: PowerShell -ExecutionPolicy Bypass -File .\run-wsl.ps1

Write-Host "=================================================" -ForegroundColor Green
Write-Host "  b1nix build helper  (Windows → Arch Linux WSL)  " -ForegroundColor Green
Write-Host "=================================================" -ForegroundColor Green

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

# Helper function to install kernel/OS build dependencies
function Install-WslDependencies {
    Write-Host "`nInstalling OS-build dependencies in Arch Linux (WSL)..." -ForegroundColor Cyan
    Write-Host "Running installation as root inside WSL..." -ForegroundColor Yellow
    wsl -d $distro -u root pacman -Syu --needed --noconfirm lld llvm limine xorriso xxd mtools dosfstools
    if ($LASTEXITCODE -eq 0) {
        Write-Host "All dependencies installed successfully!" -ForegroundColor Green
    } else {
        Write-Host "Error installing dependencies. Please run this command manually in Arch Linux:" -ForegroundColor Red
        Write-Host "  pacman -Syu --needed lld limine xorriso xxd mtools dosfstools" -ForegroundColor Yellow
    }
}

# Helper function to install GCC toolchain build dependencies
function Install-GccBuildDeps {
    Write-Host "`nInstalling GCC build dependencies in Arch Linux (WSL)..." -ForegroundColor Cyan
    Write-Host "Running installation as root inside WSL..." -ForegroundColor Yellow
    # GCC cross-compile needs: gcc make python3 curl patch texinfo gmp mpfr libmpc zlib
    wsl -d $distro -u root pacman -Syu --needed --noconfirm `
        base-devel gcc make python3 curl patch texinfo `
        gmp libmpc mpfr zlib
    if ($LASTEXITCODE -eq 0) {
        Write-Host "GCC build dependencies installed successfully!" -ForegroundColor Green
    } else {
        Write-Host "Error installing GCC build deps. Please run manually in Arch Linux:" -ForegroundColor Red
        Write-Host "  pacman -Syu --needed base-devel gcc make python3 curl patch texinfo gmp libmpc mpfr zlib" -ForegroundColor Yellow
    }
}

# Check if cross toolchain already exists
function Test-CrossToolchain {
    $result = wsl -d $distro sh -c "test -f build/cross/bin/x86_64-b1nix-gcc && echo yes || echo no" 2>`$null
    return ($result -and $result.Trim() -eq "yes")
}

# Check if native toolchain already exists
function Test-NativeToolchain {
    $result = wsl -d $distro sh -c "test -f build/native_root/bin/gcc && echo yes || echo no" 2>`$null
    return ($result -and $result.Trim() -eq "yes")
}

# Check if userspace library is built
function Test-Userspace {
    $result = wsl -d $distro sh -c "test -f userspace/build/libb1nix.a && echo yes || echo no" 2>`$null
    return ($result -and $result.Trim() -eq "yes")
}

# 3. Interactive menu
$running = $true
while ($running) {
    Write-Host ""
    Write-Host "─── Kernel / OS ──────────────────────────────────" -ForegroundColor DarkGray
    Write-Host "1) Verify / Install OS build dependencies (pacman)" -ForegroundColor Gray
    Write-Host "2) Build kernel (make all)" -ForegroundColor Gray
    Write-Host "3) Run in QEMU via WSL (WSLg / GUI required)" -ForegroundColor Gray
    Write-Host "4) Run in QEMU in console mode (Headless / Serial)" -ForegroundColor Gray
    Write-Host "5) Run in QEMU via Windows-native QEMU" -ForegroundColor Gray
    Write-Host "6) Run standard smoke tests (make smoke-x86_64)" -ForegroundColor Gray
    Write-Host "7) Run graphics smoke tests (make graphics-smoke)" -ForegroundColor Gray
    Write-Host "8) Clean build files (make clean)" -ForegroundColor Gray
    Write-Host "─── Userspace ────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host "u1) Build userspace  (libb1nix.a + binaries)" -ForegroundColor Magenta
    Write-Host "u2) Install userspace into rootfs" -ForegroundColor Magenta
    Write-Host "u3) Build full ISO with userspace  (make iso-full)" -ForegroundColor Magenta
    Write-Host "u4) Clean userspace build" -ForegroundColor Magenta
    Write-Host "─── GCC Port ─────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host "g1) Install GCC build dependencies" -ForegroundColor Cyan
    Write-Host "g2) Build cross-toolchain  (x86_64-b1nix-gcc)" -ForegroundColor Cyan
    Write-Host "g3) Build native toolchain (gcc inside b1nix)" -ForegroundColor Cyan
    Write-Host "g4) Clean toolchain build dir" -ForegroundColor Cyan
    Write-Host "─────────────────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host "q) Exit" -ForegroundColor Gray
    
    $choice = Read-Host "`nEnter option number"
    
    switch ($choice) {
        "1" {
            $missing = @()
            $tools = @("clang", "ld.lld", "make", "limine", "xorriso", "mke2fs", "xxd")
            foreach ($t in $tools) {
                if (-not (Test-WslTool $t)) {
                    $missing += $t
                }
            }
            
            if ($missing.Count -eq 0) {
                Write-Host "All build tools (clang, lld, make, limine, xorriso, mke2fs, xxd) are found in WSL!" -ForegroundColor Green
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
            wsl -d $distro make run-x86_64
        }
        
        "4" {
            Write-Host "`nRunning b1nix in console mode (Headless, output to terminal)..." -ForegroundColor Cyan
            if (-not (Test-Path "build/x86_64/b1nix.iso") -or -not (Test-Path "build/x86_64/root.ext4")) {
                Write-Host "Images not found. Building first..." -ForegroundColor Yellow
                wsl -d $distro make iso userspace-install root-image
            }
            if ($LASTEXITCODE -eq 0) {
                wsl -d $distro sh -c "cp build/x86_64/b1nix.iso /tmp/b1nix-run.iso && qemu-system-x86_64 -cdrom /tmp/b1nix-run.iso -serial stdio -display none -monitor none -no-reboot -boot d -drive file=build/x86_64/root.ext4,format=raw,if=virtio -netdev user,id=n0 -device virtio-net-pci,netdev=n0"
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
                if (-not (Test-Path "build/x86_64/b1nix.iso") -or -not (Test-Path "build/x86_64/root.ext4")) {
                    Write-Host "Images not found. Building in WSL first..." -ForegroundColor Yellow
                    wsl -d $distro make iso userspace-install root-image
                }
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "Launching QEMU..." -ForegroundColor Green
                    qemu-system-x86_64.exe -cdrom build/x86_64/b1nix.iso -serial stdio -no-reboot -boot d -drive file=build/x86_64/root.ext4,format=raw,if=virtio -netdev user,id=n0 -device virtio-net-pci,netdev=n0
                }
            }
        }
        
        "6" {
            Write-Host "`nRunning standard smoke tests..." -ForegroundColor Cyan
            wsl -d $distro make smoke-x86_64
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

        # ── Userspace ─────────────────────────────────────────────────────
        "u1" {
            Write-Host "`n── Building userspace (libb1nix.a + all binaries) ──" -ForegroundColor Magenta
            wsl -d $distro make userspace
            if ($LASTEXITCODE -eq 0) {
                Write-Host "`n Userspace built successfully!" -ForegroundColor Green
                Write-Host " Library : userspace/build/libb1nix.a" -ForegroundColor Green
                Write-Host " Binaries: userspace/build/bin/" -ForegroundColor Green
            } else {
                Write-Host "`n Userspace build FAILED." -ForegroundColor Red
            }
        }

        "u2" {
            Write-Host "`n── Installing userspace into rootfs ──" -ForegroundColor Magenta
            if (-not (Test-Userspace)) {
                Write-Host "Userspace not built yet, building first..." -ForegroundColor Yellow
            }
            wsl -d $distro make userspace-install
            if ($LASTEXITCODE -eq 0) {
                Write-Host "`n Userspace installed into build/x86_64/rootfs/" -ForegroundColor Green
                Write-Host "   bin/    - executables" -ForegroundColor Gray
                Write-Host "   lib/    - libb1nix.a + crt0.o" -ForegroundColor Gray
                Write-Host "   include/ - headers" -ForegroundColor Gray
            } else {
                Write-Host "`n Install FAILED." -ForegroundColor Red
            }
        }

        "u3" {
            Write-Host "`n── Building full ISO with userspace (iso-full) ──" -ForegroundColor Magenta
            wsl -d $distro make iso-full
            if ($LASTEXITCODE -eq 0) {
                Write-Host "`n ISO built successfully!" -ForegroundColor Green
                Write-Host " Image: build/x86_64/b1nix.iso" -ForegroundColor Green
            } else {
                Write-Host "`n iso-full FAILED." -ForegroundColor Red
            }
        }

        "u4" {
            Write-Host "`nCleaning userspace build (userspace/build/)..." -ForegroundColor Magenta
            wsl -d $distro make -C userspace clean
            if ($LASTEXITCODE -eq 0) {
                Write-Host " Userspace build dir cleaned." -ForegroundColor Green
            }
        }

        # ── GCC Port ──────────────────────────────────────────────────────
        "g1" {
            Install-GccBuildDeps
        }

        "g2" {
            Write-Host "`n-- Building cross-toolchain (x86_64-b1nix-gcc) --" -ForegroundColor Cyan
            $doRun = $true
            if (Test-CrossToolchain) {
                Write-Host "Cross-toolchain already built at build/cross/bin/x86_64-b1nix-gcc" -ForegroundColor Green
                $rebuild = Read-Host "Force rebuild? (y/N)"
                if ($rebuild -ne 'y' -and $rebuild -ne 'Y') { $doRun = $false }
            }
            if ($doRun) {
                Write-Host "This will download GCC 13.2.0 + Binutils 2.41, apply b1nix patches, and compile." -ForegroundColor Yellow
                Write-Host "Estimated time: 15-40 minutes depending on CPU core count." -ForegroundColor Yellow
                $confirm = Read-Host "Continue? (Y/n)"
                if ($confirm -ne 'n' -and $confirm -ne 'N') {
                    Write-Host ""
                    wsl -d $distro bash tools/toolchain/build-toolchain.sh
                    if ($LASTEXITCODE -eq 0) {
                        Write-Host "`n Cross-toolchain built successfully!" -ForegroundColor Green
                        Write-Host " Compiler: build/cross/bin/x86_64-b1nix-gcc" -ForegroundColor Green
                    } else {
                        Write-Host "`n Cross-toolchain build FAILED. Check output above." -ForegroundColor Red
                        Write-Host " Tip: run option g1 first to install dependencies." -ForegroundColor Yellow
                    }
                }
            }
        }

        "g3" {
            Write-Host "`n-- Building native toolchain (GCC running inside b1nix) --" -ForegroundColor Cyan
            if (-not (Test-CrossToolchain)) {
                Write-Host "[Error] Cross-toolchain not found. Run g2 first to build x86_64-b1nix-gcc." -ForegroundColor Red
            } else {
                $doRun = $true
                if (Test-NativeToolchain) {
                    Write-Host "Native toolchain already built at build/native_root/bin/gcc" -ForegroundColor Green
                    $rebuild = Read-Host "Force rebuild? (y/N)"
                    if ($rebuild -ne 'y' -and $rebuild -ne 'Y') { $doRun = $false }
                }
                if ($doRun) {
                    Write-Host "Building GCC + Binutils targeting b1nix as host..." -ForegroundColor Yellow
                    Write-Host "Estimated time: 20-60 minutes." -ForegroundColor Yellow
                    $confirm = Read-Host "Continue? (Y/n)"
                    if ($confirm -ne 'n' -and $confirm -ne 'N') {
                        Write-Host ""
                        wsl -d $distro bash tools/toolchain/build-native-toolchain.sh
                        if ($LASTEXITCODE -eq 0) {
                            Write-Host "`n Native toolchain built successfully!" -ForegroundColor Green
                            Write-Host " Output: build/native_root/" -ForegroundColor Green
                            Write-Host " Copy into rootfs image to use gcc inside b1nix." -ForegroundColor Cyan
                        } else {
                            Write-Host "`n Native toolchain build FAILED. Check output above." -ForegroundColor Red
                        }
                    }
                }
            }
        }

        "g4" {
            Write-Host "`nCleaning toolchain build directory (build/toolchain_build)..." -ForegroundColor Cyan
            $confirm = Read-Host "This removes downloaded sources and compiled objects. Continue? (y/N)"
            if ($confirm -eq 'y' -or $confirm -eq 'Y') {
                wsl -d $distro sh -c "rm -rf build/toolchain_build"
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "Toolchain build dir removed." -ForegroundColor Green
                    Write-Host "Note: build/cross and build/native_root are kept." -ForegroundColor Gray
                }
            } else {
                Write-Host "Cancelled." -ForegroundColor Gray
            }
        }

        "q" {
            $running = $false
        }
        
        default {
            Write-Host "Invalid choice. Please enter 1-8, u1-u4, g1-g4, or q." -ForegroundColor Yellow
        }
    }
}
