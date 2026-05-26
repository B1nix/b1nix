#!/bin/bash
set -ex

echo "Applying missed GCC patches..."
cd /root/b1nix-toolchain/src

python3 '/mnt/c/Users/Dmytro Manko/Documents/GitHub/b1nix/tools/patch-gcc.py' /root/b1nix-toolchain/src/gcc-13.2.0

cd gcc-13.2.0
sed -i 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
[ -f libgcc/config.sub ] && sed -i 's/| fiwix\*/| fiwix* | b1nix*/g' libgcc/config.sub

for f in libcody/*.cc libcody/*.hh; do
    [ -f "$f" ] && perl -i -pe 's/\bu8"/"/g' "$f"
done

echo "Done patching. Re-running build..."
cd /root/b1nix-toolchain/src
rm -rf build-gcc

cd '/mnt/c/Users/Dmytro Manko/Documents/GitHub/b1nix'
bash tools/build-toolchain.sh
