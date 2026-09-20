# tests/programs/bin

Programs are grouped by purpose, not milestone. Both Makefiles look a source up
by name (`$(call bin_src,NAME)` in `tests/programs/Makefile`, `$(call user_bin_src,NAME)`
at top level), so moving a file between directories needs no build change.

| Directory | Contents |
|---|---|
| `smoke/` | Smoke tests (`mNN_smoke.c`) whose markers `tests/smoke.sh` greps for |
| `gfx/` | Graphics tests and the renderers they drive |
| `helpers/` | Fixtures and targets that tests drive (`hello`, `return_42`, `m30_pie`, `m31_setuid`, `m108shell`, `m92_*` musl diagnostics) |
| `stress/` | Stress drivers (`cpustress`, `memstress`, `netstress`, ...) sharing `stress.h` |
| `tools/` | kernel diagnostics and benchmarks (`diskbench`, `fsbench`, `gpuinfo`, `mdcreate`, `selfhost_build`) |
| `compiler/` | The b1cc corpus (`b1cc_*`) the in-tree C compiler must build and run |

Coreutils are not here: BusyBox provides them, per
`tools/image/applet-manifest.conf`. Anything in `tools/` exists because
BusyBox has no applet for it.
