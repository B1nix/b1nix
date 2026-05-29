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

## Remaining M27 items (planned)

users/passwords/login · usage docs · no-graphics first-class · first-boot
persistent-root setup · POSIX compatibility matrix.
