#!/usr/bin/env bash
set -ex
mkdir -p /root/b1nix-toolchain/src/build-target-libstdcxx
cd /root/b1nix-toolchain/src/build-target-libstdcxx
export PATH="/root/b1nix-toolchain/cross/bin:$PATH"

../gcc-13.2.0/libstdc++-v3/configure \
    CC=x86_64-b1nix-gcc \
    CXX=x86_64-b1nix-g++ \
    AR=x86_64-b1nix-ar \
    RANLIB=x86_64-b1nix-ranlib \
    --build=x86_64-pc-linux-gnu \
    --host=x86_64-b1nix \
    --target=x86_64-b1nix \
    --prefix=/root/b1nix-toolchain/cross \
    --with-sysroot=/root/b1nix-toolchain/sysroot \
    --disable-multilib \
    --disable-shared \
    CFLAGS="-isystem /root/b1nix-toolchain/sysroot/include" \
    CXXFLAGS="-isystem /root/b1nix-toolchain/sysroot/include"

make -j$(nproc)
make install
