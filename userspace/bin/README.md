# userspace/bin — layout

Programs are grouped by what they are *for*, not by milestone. A file's category
is not part of its build rule: both Makefiles look a source up by name
(`$(call bin_src,NAME)` in `userspace/Makefile`, `$(call user_bin_src,NAME)` in
the top-level one), so moving a program between these directories needs no build
change.

| Directory | What lives here |
|---|---|
| `smoke/` | Kernel and POSIX smoke tests — the `mNN_smoke.c` programs whose markers `tests/smoke.sh` greps for. |
| `gfx/` | The graphics and browser stack: M47–M53, M59, M91, M101. Tests and the renderers they drive. |
| `helpers/` | Programs a test *drives* rather than a test itself: fixtures (`hello`, `return_42`), target ELFs (`m30_pie`, `m31_setuid`, `m108shell`), stress drivers, and the musl bring-up diagnostics (`m92_*`). |
| `tools/` | b1nix-specific utilities that ship in the image: `bpkg`, `b1nix_install`, `telinit`, `b1fetch`, `gpuinfo`, `diskbench`, `mc`, `ne`, `selfhost_build`. |
| `compiler/` | The b1cc corpus (`b1cc_*`) — small programs the in-tree C compiler must build and run. |

## What is *not* here

Coreutils. Every command that BusyBox already provides is a symlink to the
multicall ELF, driven by `tools/configs/applet-manifest.conf` — b1nix does not
ship a second implementation of `chmod`, `id` or `halt`. Anything under `tools/`
exists because BusyBox has no applet for it.
