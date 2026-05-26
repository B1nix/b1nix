#!/usr/bin/env python3
import os
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gcc_dir = os.path.join(PROJECT_DIR, "build/toolchain_build/gcc-13.2.0")

if not os.path.exists(gcc_dir):
    print(f"Error: {gcc_dir} does not exist", file=sys.stderr)
    sys.exit(1)

# 1. Modify gcc/config.gcc
config_gcc_path = os.path.join(gcc_dir, "gcc/config.gcc")
with open(config_gcc_path, "r") as f:
    content = f.read()

# Add target config
if "*-*-b1nix*" not in content:
    target_case = """    *-*-vxworks7*)
      # VxWorks 7 always has init/fini_array support and it is simpler to
      # just leverage this, sticking to what the system toolchain does:
      gcc_cv_initfini_array=yes
      ;;
  esac
  ;;"""
    replacement = target_case + """\n*-*-b1nix*)
  gas=yes
  gnu_ld=yes
  default_use_cxa_atexit=no
  use_gcc_stdint=wrap
  ;;"""
    content = content.replace(target_case, replacement)

    x86_case = """x86_64-*-elf*)
	tm_file="${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h"
	;;"""
    x86_replacement = x86_case + """\nx86_64-*-b1nix*)
	tm_file="${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h b1nix.h"
	tmake_file="${tmake_file} i386/t-i386elf t-fdpbit"
	;;"""
    content = content.replace(x86_case, x86_replacement)

    with open(config_gcc_path, "w") as f:
        f.write(content)

# 2. Create gcc/config/b1nix.h
b1nix_h_path = os.path.join(gcc_dir, "gcc/config/b1nix.h")
os.makedirs(os.path.dirname(b1nix_h_path), exist_ok=True)
with open(b1nix_h_path, "w") as f:
    f.write("""#ifndef GCC_B1NIX_H
#define GCC_B1NIX_H

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()      \\
  do {                                \\
    builtin_define_std ("b1nix");     \\
    builtin_define_std ("unix");      \\
    builtin_assert ("system=b1nix");  \\
    builtin_assert ("system=unix");   \\
  } while (0)

#undef STANDARD_STARTFILE_PREFIX
#define STANDARD_STARTFILE_PREFIX "/lib/"

#endif
""")

# 3. Modify libgcc/config.host
config_host_path = os.path.join(gcc_dir, "libgcc/config.host")
with open(config_host_path, "r") as f:
    content = f.read()

if "x86_64-*-b1nix*" not in content:
    host_case = """case ${host} in
aarch64*-*-elf | aarch64*-*-rtems*)"""
    replacement = """case ${host} in
x86_64-*-b1nix*)
	extra_parts="$extra_parts crtbegin.o crtend.o"
	tmake_file="$tmake_file i386/t-crtstuff t-fdpbit"
	;;
aarch64*-*-elf | aarch64*-*-rtems*)"""
    content = content.replace(host_case, replacement)
    with open(config_host_path, "w") as f:
        f.write(content)

# 4. Modify gcc/system.h
system_h_path = os.path.join(gcc_dir, "gcc/system.h")
with open(system_h_path, "r") as f:
    content = f.read()

# We need to move the C++ includes above safe-ctype.h inclusion
if '#include "safe-ctype.h"' in content:
    cpp_block = """#ifdef __cplusplus
#if defined (INCLUDE_ALGORITHM) || !defined (HAVE_SWAP_IN_UTILITY)
# include <algorithm>
#endif
#ifdef INCLUDE_LIST
# include <list>
#endif
#ifdef INCLUDE_MAP
# include <map>
#endif
#ifdef INCLUDE_SET
# include <set>
#endif
#ifdef INCLUDE_VECTOR
# include <vector>
#endif
#ifdef INCLUDE_ARRAY
# include <array>
#endif
#ifdef INCLUDE_FUNCTIONAL
# include <functional>
#endif
# include <cstring>
# include <initializer_list>
# include <new>
# include <utility>
# include <type_traits>
#endif"""
    
    content = content.replace(cpp_block, "")
    
    string_block = """#ifdef __cplusplus
#ifdef INCLUDE_STRING
# include <string>
#endif
#endif"""
    
    new_string_block = """#ifdef __cplusplus
#ifdef INCLUDE_STRING
# include <string>
#endif
#if defined (INCLUDE_ALGORITHM) || !defined (HAVE_SWAP_IN_UTILITY)
# include <algorithm>
#endif
#ifdef INCLUDE_LIST
# include <list>
#endif
#ifdef INCLUDE_MAP
# include <map>
#endif
#ifdef INCLUDE_SET
# include <set>
#endif
#ifdef INCLUDE_VECTOR
# include <vector>
#endif
#ifdef INCLUDE_ARRAY
# include <array>
#endif
#ifdef INCLUDE_FUNCTIONAL
# include <functional>
#endif
# include <cstring>
# include <initializer_list>
# include <new>
# include <utility>
# include <type_traits>
#endif"""

    content = content.replace(string_block, new_string_block)
    with open(system_h_path, "w") as f:
        f.write(content)

print("GCC patched successfully via python script!")
