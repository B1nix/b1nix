# ABI gaps

What the kernel does not implement, framed by what it costs a distribution.
The milestone list in [roadmap.md](roadmap.md) says what is planned; this file
says which Debian package or workflow each gap breaks, so that priorities come
from consequences rather than from taste.

Every entry needs three things, or it is not an entry: the gap, the thing that
breaks because of it, and how that was observed. "Probably missing" belongs in
an issue, not here.

## How a gap gets here

1. Something in the distribution fails — a unit, a package, an installer step.
2. The cause is traced to a kernel behaviour: an unimplemented syscall, a
   missing `/proc` or `/sys` file, an ioctl, a flag accepted but ignored.
3. The entry is written with the observation attached (the log line, the lane,
   the command).
4. It leaves when the milestone closes and the lane that caught it is green.

The kernel logs unmapped syscalls; that log line, when a distribution
workload trips it, is the highest-value input to this file. A quiet log during
a full Plasma session is what M124 used as its own proof.

## Open gaps

| Gap | What it breaks | Milestone | Observed |
|---|---|---|---|
| io_uring: `RECV_ZC`, io-wq affinity, NAPI busy-poll, buffer cloning, ring resizing, memory regions and the query interface; `SEND_ZC` copies rather than pinning the caller's pages | A program that needs one of these; everything else this uapi names works (M125), and each absence is refused at setup or reported by `IORING_REGISTER_PROBE` | M133 | liburing's suite in the Debian lane names them |
| perf: tracepoints, kprobes, and eBPF with a JIT, BTF or CO-RE | `bpftrace` and any CO-RE toolchain; `perf stat`/`record`/`report` on hardware and software counters work (M126), and each absence is refused at load or open with the reason | M133 | `m126_bpf_smoke` in the posix lane, and the distribution's own perf in the Debian lane |
| Suspend (ACPI S3), device suspend/resume ordering | closing a laptop lid and having it wake with its devices; s2idle, cpufreq and cpuidle are done | M129 | — |
| Wi-Fi (mac80211/cfg80211, nl80211) | `iw`, `wpa_supplicant`, iwd, NetworkManager on anything wireless | M130 | — |
| The kernel is not relocatable, so it cannot boot under UEFI | every UEFI machine, which is every machine sold in the last fifteen years; the distribution boots through Limine's BIOS path instead | M133 | Limine answers `PANIC: multiboot2: Could not find viable load address for executable` under OVMF. The multiboot2 header asks for a fixed load at 1 MiB and the firmware is already there. The tree's own ISOs fail identically, so this is the kernel and not the image. Fixing it means a relocatable kernel (multiboot2 tag 10, and page tables that do not assume 0x100000) |
| AHCI probe hangs on a port with an empty ATAPI device | booting on QEMU's q35, which always carries an ICH9 AHCI controller | M133 | The boot stops dead after `ahci: port 2 ready (packet device)`. The soak notes have carried this one for a while; the distribution lanes work around it with `-machine pc` |
| A mount is reported to `/proc/self/mountinfo` under the path it was recorded with, not the path the caller used | Debian's `tmp.mount` and `run-lock.mount` fail with `Result: protocol` — systemd mounts through `/proc/self/fd/N`, watches for `/tmp` to appear, and never sees it | M133 | Observed in `DISTRO-SMOKE`. The mount itself succeeds; only its name is wrong. The same one-node-two-names knot that `vfs_set_propagation` and `vfs_remount` now solve by matching on the node |
| `sched_setscheduler` refuses what systemd asks for | `e2scrub_reap.service` exits `214/SETSCHEDULER` | M133 | Any unit with `CPUSchedulingPolicy=` hits it |
| A `file:` repository cannot be read by apt | `apt-get update` against a local mirror: `Symlinking file  to …/Packages.zst failed (22)`, then `Failed to fetch store:…Packages Read error (22)` | M133 | Observed in `DISTRO-SMOKE` over a 9p share. Two suspects: `symlink()` with an empty target, and whatever apt's `store:` method does to read an index |
| `systemd-sysusers` fails | user and group creation at boot | M133 | Last of the four units still failing after the mount fixes |
| MTD/UBI | A handful of BusyBox applets | M107 | `wontfix`: no hardware in scope needs it |
| DKMS-built out-of-tree modules | nvidia, virtualbox, zfs from Debian | — | Not planned: headers are shipped, the modules are not supported |

## Closed, kept for the pattern

These are the shapes of failure that recur, and knowing them shortens the next
hunt:

- **A syscall that exists but lies.** The M124 work found calls that returned
  success without doing the thing; the distribution then failed somewhere
  unrelated. Probing by result *and* errno, as the Debian lane does, is what
  catches this.
- **A `/proc` or `/sys` file whose content is the API.** `nlink` on a procfs
  directory, `/proc/pressure/*`, `/proc/self/fd/N` — userspace parses these,
  and a plausible-looking wrong value is worse than an absent file.
- **A flag accepted and ignored.** Accepting `MSG_DONTWAIT` or an `O_` flag
  without honouring it turns a clean error into a hang somewhere else.
- **One rejected option name, a dozen dead units.** trixie's `mount(8)` and
  systemd both go through the new mount API, where every option is a string
  handed to `fsconfig`. The context parser knew `size` and `nr_inodes` but not
  `strictatime`, which Debian's `tmp.mount`, `run-lock.mount` and every tmpfs
  systemd builds for a unit's mount namespace pass. The result was `/tmp`,
  `/run/lock`, `systemd-logind` and `systemd-udevd` all failing, reported as
  "Failed to set up mount namespacing: Invalid argument" — a message naming
  neither the option nor the filesystem. An option belonging to the VFS rather
  than to a filesystem has to be accepted on every type.
- **A wrong identifier.** The netlink port id taken for a process id cost a
  full debugging session in the Debian kernel-swap work.

## Where to look when something breaks

- The unmapped-syscall log line, first, always.
- `strace` under the Debian lane — the distribution's own `strace` runs.
- `docs/kernel/processes-and-system-calls.md` for the per-call state.
- The Debian and Plasma lanes' logs in `smoke_run/`, read whole with `grep -a`.
