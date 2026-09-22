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
| Wi-Fi (mac80211/cfg80211, nl80211) | `iw`, `wpa_supplicant`, iwd, NetworkManager on anything wireless | M130 | — |
| The kernel is not relocatable, so it cannot boot under UEFI | every UEFI machine, which is every machine sold in the last fifteen years; the distribution boots through Limine's BIOS path instead | M133 | Limine answers `PANIC: multiboot2: Could not find viable load address for executable` under OVMF. The multiboot2 header asks for a fixed load at 1 MiB and the firmware is already there. The tree's own ISOs fail identically, so this is the kernel and not the image. Fixing it means a relocatable kernel (multiboot2 tag 10, and page tables that do not assume 0x100000) |
| Debian's `tmp.mount` and `run-lock.mount` fail with `Result: protocol` | /tmp and /run/lock on an installed system; the boot reaches `multi-user.target` regardless since the mount-table notification was fixed | M133 | Observed in `DISTRO-SMOKE`. The mount itself succeeds and `/proc/self/mountinfo` names it correctly (a mount made by hand, in the same boot, is found), so what remains is between the notification and systemd's own bookkeeping: it runs `mount(8)`, the mount appears, and it still reports "Mount process finished, but there is no mount" |
| A `file:` repository cannot be read by apt | `apt-get update` against a local mirror: `Failed to fetch store:…Packages Read error (22: Invalid argument)` | M133 | Observed in `DISTRO-SMOKE` over a 9p share. The first half is closed: apt's `Symlinking file  to …Packages.zst failed` now reports ENOENT, which is what Linux answers for an empty target and what apt's fallback expects. What remains is the `store:` method's read. Not the 9p transport -- a file larger than any 9p message reads whole on the same share (`M110-9P: ok read-big`) -- and not apt giving up early, since it then reports the index missing |
| No ACPI events: no SCI handler, no GPE dispatch, no `Notify` | Nothing the platform raises reaches userspace — a lid close cannot suspend the machine, a power button press does nothing, and `upower`/`systemd-logind` see a battery that only changes when they poll it | M135 | The kernel has no SCI vector at all; `/sys/class/power_supply/BAT0` is re-evaluated on read and never announces a change |
| `reboot(RB_POWER_OFF)` writes hard-coded QEMU/Bochs ports rather than `\_S5` through the FADT's PM1 control register | Powering off a real machine: the kernel prints "poweroff unsupported, halting" and leaves it running | M135 | `kernel/syscall/syscall.c` writes 0x604/0xB004/0x4004; the S3 path already reads the registers this needs |
| cpufreq has no load-driven governor, no `policy*` layout and ignores `_PPC` | `cpupower`, `tuned` and every desktop power profile: the clock only moves when something writes a governor by hand, and the firmware's own ceiling on battery is not honoured | M135 | `/sys/devices/system/cpu/cpu0/cpufreq` offers `performance powersave` and no policy directory |
| No hibernate (S4) and no `/sys/power/{disk,wakeup_count,mem_sleep,wakeup_sources}` | `systemctl hibernate`, and anything that counts wakeups before suspending (`systemd-sleep`'s race avoidance) | M135 | `/sys/power` has one file, `state` |
| Thermal zones are published but nothing acts on them: no trip points, no cooling devices, no critical shutdown | A machine that overheats keeps running; `thermald` and the kernel's own throttling have nothing to work with | M135 | `/sys/class/thermal/thermal_zoneN` carries `type` and `temp` only |
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
- **An operation allowed only on files.** Record locks, `SO_PASSCRED`, and
  everything else Linux handles generically above the filesystem apply to any
  open file description — a socket, a pipe, an eventfd. Refusing them anywhere
  else with EBADF is the shape that broke `PrivateNetwork=`.
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
  full debugging session in the Debian kernel-swap work. The same shape put
  `e2scrub_reap.service` in this table under `sched_setscheduler`: its exit
  status was read as 214 (EXIT_SETSCHEDULER) when it was 225 (EXIT_NETWORK),
  and the call that failed was `fcntl(F_OFD_SETLK)` on the socketpair systemd
  stores a network namespace in. An exit status is a number until systemd's own
  table is asked what it means, which is why the lane now asks.
- **A probe that cannot give up.** The I/O path may wait for ever — its buffer
  belongs to a caller and abandoning the wait frees memory the controller may
  still write into. A probe may not: an ATAPI port with no disc never completes
  READ CAPACITY, and the AHCI probe waited for it for ever, so every q35 boot
  with an empty optical drive stopped before userspace. A bounded wait that
  stops the port before the buffer goes back has neither problem.

## Where to look when something breaks

- The unmapped-syscall log line, first, always.
- `strace` under the Debian lane — the distribution's own `strace` runs.
- `docs/kernel/processes-and-system-calls.md` for the per-call state.
- The Debian and Plasma lanes' logs in `smoke_run/`, read whole with `grep -a`.
