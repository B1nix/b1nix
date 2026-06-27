# CMake toolchain: build clang as native Linux binary with b1nix default target.
# NOT a cross-compile — produces a Linux ELF that defaults to targeting b1nix.

set(CMAKE_C_FLAGS "-D__linux__=1")
set(CMAKE_CXX_FLAGS "-D__linux__=1")
