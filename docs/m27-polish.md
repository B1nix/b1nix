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

## Remaining M27 items (planned)

init scripts + service supervisor · users/passwords/login · stable
shutdown/reboot/emergency shell (note: `SYS_REBOOT` is currently a no-op stub
that just `arch_halt()`s) · usage docs · no-graphics first-class · first-boot
persistent-root setup · POSIX compatibility matrix.
