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
| Wi-Fi (mac80211/cfg80211, nl80211) | `iw`, `wpa_supplicant`, iwd, NetworkManager on anything wireless | M130 | — |
| PSI triggers: `cpu/memory/io.pressure` are read-only, a threshold cannot be written and polled | systemd's `MemoryPressureWatch=` and `sd_event_add_memory_pressure()` (journald, logind, udevd shed caches on pressure); oomd is unaffected, it polls the averages | — | The files exist in every cgroup and their averages move (oomd kills on them on the systemd lane), but they are created without a write handler, so there is nothing for a trigger to arm |
| A rare boot wedge: tasks READY inside the block layer's admission wait are never picked while every CPU sits in cpuidle | The boot stops (about one boot in fifteen on the distribution image; the fourth fallback boot of DISTRO-SMOKE caught it) | — | gdb on the wedged guest: two tasks READY in `blk_io_begin`'s yield loop, readers behind them on `folio_lock`, CPUs 1–3 halted in `cpuidle_enter` and CPU 0 looping in `scheduler_yield_inner` without picking them; not reproduced in 50 further boots |
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

- **A queue that drops silently.** A netlink socket held sixteen messages
  whatever `SO_RCVBUF` said. Coldplug announces every device at once before
  udevd is reading, so the seventeenth `add` -- the root partition's -- was
  lost, its device unit never became plugged, and the boot waited behind it. A
  full queue has to grow to the limit the program asked for and then say
  `ENOBUFS`, never shrug.
- **A notification for the wrong audience.** `/proc/self/mountinfo` changed
  for every mount in every namespace, so each service's private credential
  mounts woke PID 1. sd-event rate-limited the mount monitor and systemd held
  back every mount job while it was limited. Linux scopes the event to the
  reader's namespace; so must anything modelled on it. The same boot showed the
  sibling bug: a nested epoll that ignored `EPOLLET` called libmount's epoll
  ready for ever.

## Where to look when something breaks

- The unmapped-syscall log line, first, always.
- `strace` under the Debian lane — the distribution's own `strace` runs.
- `docs/kernel/processes-and-system-calls.md` for the per-call state.
- The Debian and Plasma lanes' logs in `smoke_run/`, read whole with `grep -a`.
