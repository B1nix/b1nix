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
| cgroup v2 controllers (memory, cpu, io, pids), PSI | systemd slices, `MemoryMax`/`CPUWeight`, `systemd-oomd`, `systemctl status` accounting, container limits under podman | M127 | systemd runs as PID 1 (M112) but the resource-control half of its model has nothing behind it |
| io_uring | Modern userspace that assumes it; a distribution QEMU and `fio` | M125 | — |
| `perf_event_open`, eBPF, kprobes | `perf record`/`perf top`, `bpftrace`, anything in Debian that profiles | M126 | — |
| `userfaultfd`, `fanotify` | CRIU, live migration, file-access monitoring | M126 | — |
| Suspend (s2idle, S3), cpufreq, cpuidle | `systemctl suspend`, battery life, a laptop that can be closed | M129 | — |
| Wi-Fi (mac80211/cfg80211, nl80211) | `iw`, `wpa_supplicant`, iwd, NetworkManager on anything wireless | M130 | — |
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
- **A wrong identifier.** The netlink port id taken for a process id cost a
  full debugging session in the Debian kernel-swap work.

## Where to look when something breaks

- The unmapped-syscall log line, first, always.
- `strace` under the Debian lane — the distribution's own `strace` runs.
- `docs/kernel/processes-and-system-calls.md` for the per-call state.
- The Debian and Plasma lanes' logs in `smoke_run/`, read whole with `grep -a`.
