# M34–M36: Diagnostics, Core Dumps, and Kernel Debugging

Closure of milestones M34 (virtual filesystems), M35 (core dumps & symbolication),
and M36 (GDB stub & tracing). M33 (POSIX shell) was already closed.

## M34 — /proc and /sys

`kernel/fs/procfs.c`, `kernel/fs/sysfs.c`.

Synthetic pseudo-files are **`VFS_DEVICE` nodes carrying a `read_cb`**, not
`VFS_FILE`. This is deliberate: the VFS read path routes `VFS_FILE`+`read_cb`
through the demand-paging page cache (keyed by inode), which would freeze the
first read's bytes — wrong for `/proc/uptime`, `/proc/meminfo`, etc. The
`VFS_DEVICE`+`read_cb` path instead calls `read_cb(offset, size)` on every
read, regenerating content live.

Path resolution (`find_child`) walks physical child nodes, so every exposed
file is linked as a real child of the procfs/sysfs root. Per-pid directories
are **materialised lazily** from the root `readdir_cb` (`procfs_refresh`): one
`/proc/<pid>` dir per live task. Their per-file `read_cb` derives the target pid
from the parent directory's name and looks the task up by pid at read time, so a
reused slot Just Works and a dead pid reports state `Z`. Dirs are never pruned
(bounded by distinct pids seen over a boot), which avoids freeing a node a reader
still holds.

`/proc/self` is a pid dir whose pid resolves to the *caller* at read time.

Files: `meminfo`, `uptime`, `loadavg`, `version`, `cpuinfo`, `stat`,
`filesystems`; `self/` and `<pid>/` → `status`, `cmdline`, `comm`, `stat`,
`maps`. `/sys`: `kernel/{ostype,osrelease,hostname,version}`,
`devices/system/cpu/{possible,online,present}`, `memory/total_kb`.

Task-table introspection added without growing `struct task` (which breaks an
unrelated paging invariant — see the M29 note): `scheduler_task_slots/slot/
by_pid/state_name` in `kernel/sched/scheduler.c`.

Tools: `free`, `top`, `sysctl` in `kernel/user/busybox.c` read `/proc` and
`/sys` (joining the existing `ps`).

## M35 — Core dumps & kallsyms

**Core dumps** (`kernel/arch/x86/coredump.c`). On a fatal CPU-fault signal with
no handler, `coredump_write(frame, sig)` runs from the exception handler — in the
dying task's still-live address space — and writes `/tmp/core` (ramfs, never
blocks). The ELF is `ET_CORE`/`EM_X86_64` with a `PT_NOTE`/`NT_PRSTATUS`
register file and one `PT_LOAD` per mapped run of the address space. Every page
is probed with `vmm_virt_to_phys()` before reading, so a lazily-unmapped page
cannot fault the dumper a second time. Bounded to 32 segments / 1 MB.

**kallsyms** (two-pass link). `linker.ld` defines an empty `.kallsyms` section
*after* `.text/.rodata/.data` (whose addresses are frozen by the existing 512K
padding). Pass 1 links `kernel.elf.stage1`; `tools/gen_kallsyms.sh` runs `nm -n`
on it and emits packed `[u64 addr][asciz name]` records; pass 2 re-links with the
blob appended. Because the blob lands after the frozen sections, the pass-1
addresses it records stay valid in the final image. `ksym_lookup`/`ksym_print`
(`kernel/lib/klog.c`) resolve `name+0xoffset`, wired into `arch_backtrace`.

## M36 — GDB stub & ftrace

**GDB stub** (`kernel/arch/x86/gdbstub.c`). Implements the GDB Remote Serial
Protocol over COM1: `?`, `g`/`G`, `m`/`M`, `c`/`s`, `qSupported`. The protocol
engine (`gdb_handle_packet`) is split from the transport (`struct
gdb_transport`) so it is unit-tested in-kernel with an in-memory transport — no
live host needed. The interactive serial loop (`gdb_stub_enter`) is entered on
int3 (#BP)/#DB **only when booted with `b1nix.gdb`**, so an ordinary boot never
blocks waiting on a host debugger.

To attach a real host: boot with `b1nix.gdb`, connect QEMU's serial to a socket,
then `gdb kernel.elf` → `target remote ...`.

**ftrace** (`kernel/lib/ftrace.c`). `__cyg_profile_func_enter/exit` record a ring
buffer of `(addr, enter|exit)` events when enabled; addresses are symbolised via
kallsyms on dump. Only opted-in translation units are instrumented — the Makefile
adds `-finstrument-functions` to `ftrace_demo.c` alone — so the kernel is not
globally slowed and the hooks (which are `no_instrument_function`) never recurse.

## Verification

Smoke markers (single-CPU and `-smp 4`): `M34-PROC: …` (13), `M35-DIAG: …` +
`M35-CORE: …` (10), `M36-GDB: …` + `M36-FTRACE: …` (10). See `tests/smoke.sh`.
