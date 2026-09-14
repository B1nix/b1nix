# Loadable kernel modules (M95/M96)

b1nix modules are relocatable ELF objects (`ET_REL`, `.ko`) built from kernel
sources with `-DMODULE`. Kernel stays monolithic; LKMs are the modularity
mechanism.

| File | Role |
|---|---|
| `kernel/include/b1nix/module.h` | `struct module`, `EXPORT_SYMBOL`, `MODULE_*` tags, `module_init/exit`, `module_param` |
| `kernel/module/module.c` | loader, relocations, unload, params, sysfs, `request_module` |
| `kernel/module/ksyms.c` | kernel `EXPORT_SYMBOL` list (single source of truth) |
| `kernel/mm/module_alloc.c` | module VA region allocator |
| `tools/build/kernel/gen_modules_initramfs.sh` | packs `.ko`s + generated `modules.dep`/`modules.alias` into the initramfs |
| `tools/build/kernel/check-module-syms.sh` | build fails if a module has an undefined symbol no kernel/module export provides |

## Building

`MODULE_NAMES` in the top-level `Makefile` (currently `isofs ntfs hda ipv6 ndp
ntp`) → `build/<arch>/modules/*.ko` → `initramfs_modules.inc` and
`/lib/modules/<release>/` in the root image. `<release>` is
`B1NIX_RELEASE_STR` (`<linux-abi>-b1nix-<version>`), matching `uname -r`.

A module source declares:

```c
MODULE_NAME("ndp");            /* mandatory: the module's identity */
MODULE_LICENSE("..."); MODULE_DESCRIPTION("...");
MODULE_ALIAS("...");           /* extra names for request_module/modprobe */
MODULE_DEPENDS("ipv6");        /* verified against the real symbol graph at build */
module_param_desc(var, MODULE_PARAM_INT, 0644, "desc");
module_init(fn); module_exit(fn);
```

`vermagic` (`B1NIX_RELEASE_STR " x86_64 SMP"` / `" aarch64 SMP"`) is emitted
automatically; a mismatch is refused. Every `.modinfo` tag starts with a NUL so
BusyBox `modinfo` can find the first one.

## Memory

- x86_64: `0xFFFFFFFFC0000000`, 128 MiB, inside the kernel's top-2 GiB PML4/PDPT
  slot 511 — shared by all address spaces, reachable by `-mcmodel=kernel`
  `R_X86_64_32S/PC32/PLT32`.
- aarch64: 16 MiB, identity-mapped, 2 MiB-aligned just past `__kernel_end`
  (`module_region_base()`), within `CALL26/JUMP26` reach.
- Page bitmap allocator (`module_alloc`, `module_free`, `module_set_prot`):
  allocated RW+NX, text switched to RX after relocation (W^X).

## Load / unload

`module_load_image()` / `module_load_path()`: parse `.modinfo` (name, vermagic),
copy allocated sections, resolve undefined symbols against kernel exports and
live modules' exports (`module_symbol_lookup`), apply relocations (x86_64
`R_X86_64_*`; aarch64 ABS/PREL/CALL26/JUMP26/ADR_PREL_PG_HI21/`*_ABS_LO12_NC`),
protect text RX, apply params, call `init`. States: `LOADING` → `LIVE` →
`UNLOADING`.

`module_unload()` refuses (`-EBUSY`) while `refcnt > 0` or not `LIVE`, then runs
`exit`, removes sysfs, unlinks under `module_lock`, frees. Users pin a module
with `try_module_get()` / `module_put()`; the VFS pins a filesystem's `owner`
for each mount.

## Interfaces

- Syscalls: `SYS_INIT_MODULE` (245), `SYS_DELETE_MODULE` (246),
  `SYS_FINIT_MODULE` (247), also mapped from the Linux ABI. Require root or
  `CAP_SYS_MODULE`; image size capped at 16 MiB.
- `/proc/modules`: `name size refcnt deps state addr` (Linux format).
- `/sys/module/<name>/parameters/<param>`: readable; writable when `perm` has
  write bits. Params can also be passed at `insmod`.
- `request_module(name)`: no-op if loaded; resolves aliases via
  `modules.alias`; loads `modules.dep` dependencies first. At boot
  `module_init_builtin_deps()` requests `isofs ntfs hda ndp ipv6 ntp` (each
  optional).
- Userspace tools are BusyBox `insmod`/`rmmod`/`lsmod`/`modprobe`/`modinfo`.

Tests: `userspace/bin/smoke/m96_smoke.c`, markers `M95-SMOKE:` / `M96-SMOKE:`
in `tests/smoke.sh`.
