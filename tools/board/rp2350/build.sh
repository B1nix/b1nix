#!/bin/sh
set -eu

# Check for Arm GNU Toolchain in Applications or PATH
for toolchain in /Applications/ArmGNUToolchain/*/arm-none-eabi/bin; do
    if [ -d "$toolchain" ]; then
        export PATH="$toolchain:$PATH"
        break
    fi
done

# Target board: pico2 (RP2350) or pico (RP2040)
PICO_BOARD="${PICO_BOARD:-pico2}"
BUILD_DIR="build-${PICO_BOARD}"

echo "================================================================"
echo " Building b1nix UART Bridge & HIL Controller for: $PICO_BOARD"
echo " Compiler: $(which arm-none-eabi-gcc)"
echo "================================================================"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DPICO_BOARD="$PICO_BOARD" ..
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo ""
echo "[✓] Build completed successfully!"
ls -lh b1nix_uart_bridge.uf2 2>/dev/null || ls -lh b1nix_uart_bridge.elf
