# M27: Terminal OS Polish

Milestone for everyday-usability polish on top of the M0–M26 core. Tracked on
branch `m27-terminal-os-polish`.

## Kernel command line parsing + boot menu options — DONE (2026-05-29)

### `bootinfo_get_kv()`
`kernel/bootinfo/multiboot2.c` gains a `key=value` token parser to complement
the existing whole-token `bootinfo_has_flag()`:

```c
int bootinfo_get_kv(const char *key, char *out, usize out_size);
```

- Matches a whole key up to `=` (so `b1nix.tes` never matches `b1nix.test=1`).
- Copies the value into `out`, always NUL-terminated, truncated to
  `out_size - 1`. `out` may be NULL / `out_size` 0 to test presence only.
- Returns 1 on match (value copied), 0 otherwise (out untouched).

### Boot options in `init_main` (`kernel/user/programs.c`)
The production (non-test) init path resolves which program to launch with this
precedence:

1. `init=<path>` — explicit override.
2. `b1nix.single` / `single` — single-user **emergency shell** (`/bin/sh`),
   skips the graphical UI.
3. graphical UI `/bin/mc` — when `b1nix.ui=1` / `ui=1` and **not**
   `b1nix.nographics` / `nographics`.
4. plain `/bin/sh` — default.

If the chosen init fails to spawn (and it isn't already `/bin/sh`), init prints
a notice and falls back to an emergency `/bin/sh` so the box is never left
without a shell.

### GRUB boot menu
`boot/grub/grub.cfg` is now a template with `@CMDLINE@` / `@TIMEOUT@`
placeholders. The `iso` Makefile target `sed`-substitutes them and emits three
menu entries:

- `b1nix` (normal)
- `b1nix (single-user / emergency shell)` — appends `b1nix.single`
- `b1nix (text mode, no graphics)` — appends `b1nix.nographics`

`GRUB_TIMEOUT` make var defaults to `0` so the smoke harness boots entry 0
immediately and never stalls. For an interactive menu, build with
`make iso GRUB_TIMEOUT=5`.

### Verification
Kernel self-test in the `b1nix.test=1` block emits `M27-CMDLINE: ok kv-parse`,
covering: present key (`b1nix.test`→`1`), present multi-char value
(`b1nix.kvtest`→`abc123`), absent key, prefix non-match, and value truncation
into a 4-byte buffer. The smoke cmdline is
`b1nix.test=1 b1nix.kvtest=abc123`. Host smoke: **223/0**.

### Known coverage gap
The smoke suite always boots `b1nix.test=1`, runs the test block, and reboots —
it **never reaches the production init path**. So `init=`, `b1nix.single`, and
`b1nix.nographics` dispatch are not auto-validated; only the parser primitive
is. Future M27 items that live on the production path (init scripts/supervisor,
login, shutdown/reboot/emergency) will need a dedicated cmdline+marker or manual
in-guest verification.

## Shutdown / reboot / emergency shell — DONE (2026-05-29)

`SYS_REBOOT` was a no-op stub (`console_write` + `arch_halt`). It now takes a
command in `arg0` (constants in both `syscall.h`):

| cmd | value | action |
|-----|-------|--------|
| `B1NIX_REBOOT_RESTART`  | 0 | drain the 8042 input buffer, pulse the reset line (`outb 0x64, 0xFE`); fall back to a triple fault via a null IDT, then `arch_halt` |
| `B1NIX_REBOOT_POWEROFF` | 1 | QEMU/Bochs ACPI shutdown ports (`0x604`, `0xB004`, `0x4004`); fall back to `arch_halt` |
| `B1NIX_REBOOT_HALT`     | 2 | `arch_halt` |

Shell commands (`kernel/user/busybox.c`, registered as `/bin/*` in
`programs.c`): `reboot` → RESTART, `poweroff`/`shutdown` → POWEROFF,
`halt` → HALT.

**Emergency shell:** `init_main` already drops to a single-user `/bin/sh` on
`b1nix.single`, and falls back to an emergency `/bin/sh` if the chosen init
fails to spawn (see the cmdline section above).

**Verification:** the test harness runs with QEMU `-no-reboot`, so the
end-of-test `SYS_REBOOT(RESTART)` performs a real machine reset and QEMU exits
cleanly. Smoke checks `reboot: restarting`; host smoke **224/0**.

**Gotcha fixed along the way:** registering the 4 new `/bin` reboot commands
overflowed `MAX_PROGRAMS` (was 64) in `kernel/user/process.c`.
`user_register_program` silently skips both the registry slot *and* the
`vfs_create` once full, which dropped `/bin` VFS entries and broke
`M26-SMOKE: ok readdir`. Bumped `MAX_PROGRAMS` to 96.

## Init scripts + service supervisor — DONE (2026-05-29)

**Boot rc script:** `/etc/rc` is shipped in the initramfs
(`kernel/fs/initramfs.c`, `initramfs_rc[]`). At startup `init_main` runs it once
via `/bin/sh /etc/rc` (after clearing the screen, before the login shell),
reusing the shell's existing script-execution path. It is a real, editable shell
script — the default prints a banner and `cat`s `/etc/motd`.

**Service supervisor:** the init reap loop now respawns the login shell whenever
it exits, so the console is never lost. This also fixes a latent busy-spin:
`scheduler_waitpid(0)` returns `-ECHILD` immediately once init has no children,
so the old `while(1) wait()` loop would spin at 100% CPU the moment the shell
exited. The supervisor keeps exactly one live shell (blocking wait), and halts
(`B1NIX_REBOOT_HALT`) rather than spin if no shell can be spawned at all.

**Verification:** a test-mode self-test runs `/bin/sh /etc/rc` (the same path
production init uses); smoke checks `M27-INIT: ok rc-script`. Host smoke
**225/0**. The supervisor's respawn loop lives on the production path and isn't
auto-exercised by the harness (which boots `b1nix.test=1` → reboots).

## Users / passwords / login basics — DONE (2026-05-29)

**`/etc/passwd`** ships in the initramfs with `root:x:0:0:...:/bin/sh` and
`user:x:1000:1000:...:/home/user:/bin/sh` (password field `x` — no shadow db /
password check yet). **libc `getpwnam`/`getpwuid`** (`userspace/libc/pwd.c`)
now parse it line-by-line (returned `struct passwd` + strings in static
storage, standard contract), falling back to a hardcoded `root` entry when the
file is absent (keeps GNU Make's `~` expansion working).

**`/bin/login`** (`login_main` in `kernel/user/busybox.c`) reads `/etc/passwd`
kernel-side, prompts for a username (or takes it as `argv[1]`), drops
privileges (`setgid`/`setuid`), `chdir`s to the home dir, and `fork`+`execve`s
the user's login shell — modelled on the existing `su`. `init` launches it when
`b1nix.login` is on the kernel command line.

**Verification:** `userspace/bin/m27_smoke.c` (`M27-USER:` markers) checks
`getpwnam("root")`/`getpwnam("user")`/`getpwuid(1000)`, NULL for an unknown
user, and a `setgid`/`setuid` privilege drop to uid 1000 in a forked child
(the exact sequence `login` performs). Host smoke **231/0**. The interactive
`login` prompt itself is on the production path and isn't auto-tested, but its
building blocks (passwd parse + privilege drop) are.

## Fixed: ramfs `getdents` returned every-other entry + duplicates

Adding the new `/bin` programs pushed `/bin` past one 32-entry `getdents`
batch and exposed a real `readdir` bug in `kernel/fs/vfs.c`: the fallback
ramfs readdir's skip used the running `offset`, which it also incremented while
emitting, so after the initial skip it alternated skip/emit — returning every
*other* entry and re-emitting already-seen ones on the next batch. It only ever
"worked" for callers whose target landed on an even index. Rewrote the walk to
use a separate absolute cursor (`idx`) vs the emitted `count`, so the resume
position is exact and multi-batch directory reads are correct. (Surfaced as a
spurious `M26-SMOKE: fail readdir` once `/bin` crossed 32 entries.) Also dropped
three pre-existing unused static locks in vfs.c to keep the build warning-free.

## No-graphics as a first-class target — DONE (2026-05-29)

b1nix is fully usable over a text/serial console with no framebuffer: all
kernel and userspace I/O goes to COM1 (the smoke harness itself runs
`-display none`), the shell, coreutils, editor (`ne`) and the toolchain are all
text-mode, and the kernel boots without a GRUB framebuffer tag. The graphical
file manager `/bin/mc` is the only GUI surface and is strictly opt-in
(`b1nix.ui=1`). `b1nix.nographics` (or `nographics`) on the kernel command line
forces text mode and is honoured by `init_main` even if `b1nix.ui=1` is also
present, so a headless boot never tries to start the GUI. The single-user
GRUB / `b1nix.single` path is likewise pure text.

## First-boot setup for persistent root images — DONE (2026-05-29)

`make root-image` builds an ext4 image that the kernel mounts at `/persist`
(from `virtio-blk0`) when present. The boot rc script (`/etc/rc`) now performs
**first-boot initialisation**: if `/persist` is mounted and the
`/persist/.b1nix-setup` marker is absent, it creates `home`/`etc`/`tmp` under
`/persist` and writes the marker, so the structure is laid down once and skipped
on every later boot. The block is guarded by `[ -d /persist ]`, so it is inert
when `/persist` is not mounted (the smoke harness attaches its own SATA/NVMe/swap
drives and no `/persist`), which is why it has no dedicated smoke marker — the
surrounding `M27-INIT: ok rc-script` still proves the script (including these
lines) parses and runs.

## Everyday usage: boot → edit → build

1. **Boot.** `make iso` then run under QEMU. GRUB shows three entries (normal,
   single-user/emergency, text-mode/no-graphics — build with `GRUB_TIMEOUT=5`
   to see the menu). Useful kernel command line options:
   `init=<path>` (override the first program), `b1nix.single` (emergency root
   shell), `b1nix.nographics` (force text mode), `b1nix.login` (login prompt),
   `b1nix.ui=1` (launch `/bin/mc`).
2. **Log in.** With `b1nix.login`, `/bin/login` prompts for a user (`root` or
   `user` from `/etc/passwd`), drops to their uid/gid and starts their shell.
   Otherwise you land directly in `/bin/sh`.
3. **Explore / edit.** Standard tools: `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`,
   `grep`, `sort`, `ps`, pipes/redirection, plus the `ne` text editor and the
   `mc` file manager. The shell supports scripts (`/bin/sh script.sh`).
4. **Build.** The ported native toolchain (`gcc`, `as`, `ld`, GNU `make` — see
   [`m26-selfhost.md`](m26-selfhost.md)) compiles and links C programs in-guest;
   `tcc` is also available. Work on `/persist` to keep results across reboots.
5. **Shut down.** `poweroff` / `halt` / `reboot` (or `shutdown`).

## POSIX compatibility matrix

See [`posix-requirements.md`](posix-requirements.md) for the authoritative
per-requirement contract and [`posix-branches.md`](posix-branches.md) for
status. High-level summary (from [`roadmap.md`](roadmap.md)): overall ~70-78%;
VFS/path ~90-95%; shell/coreutils ~80-85%. Quick matrix of the areas M27
touches:

| Area | Status | Notes |
|------|--------|-------|
| Process/signals (fork/exec/wait, kill, sigaction) | good | M12/M13 |
| Userspace ABI + libc (stdio, string, unistd, dirent, pwd) | good | M13/M26 |
| VFS / readdir / getdents | good | multi-batch readdir fixed in M27 |
| Filesystems (ext2/3/4, fat32, initramfs) | partial | no metadata_csum/64bit/flex_bg |
| Job control / termios | partial | M13-JC |
| Users / passwd / login | initial | `/etc/passwd` + `getpwnam`; no password check or shadow |
| Networking (ARP/ICMP/UDP/TCP) | partial | TCP baseline only |
| Shutdown / reboot | good | restart + ACPI poweroff (M27) |
