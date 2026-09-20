# Tree cleanup

M121 replaced this tree's own userspace with Alpine and Debian packages, and
the distribution decision keeps that direction. What is left behind is not
obviously dead: most of it is still referenced by a Makefile, so deleting it
blind breaks a lane a week later.

The rule is the same for every entry below: **prove it is dead, then delete it
outright.** Nothing moves to `archive/` — git history is the archive, and a
file sitting in `archive/` still turns up in every `grep` and still looks like
something a reader must understand.

## The proof procedure

For each candidate, in order:

1. `git grep -n <name> -- ':!archive' ':!build'` and read every hit. A hit in a
   `Makefile` variable is not proof of life: the variable may itself be dead.
2. Delete it, including the rules and variables that named it.
3. `make -j6` for both arches.
4. Run the full smoke suite in the foreground, plus the Debian lane if the
   candidate touched anything a distribution uses.
5. Green on both arches → the deletion stands, and the commit body says which
   lanes proved it. Red → restore, and record in this file what the thing is
   actually for, so the next attempt does not repeat the work.

A deletion never rides along with a feature commit. `clean:` commits only, one
subject per removed thing or per closely related group.

## Candidates

| What | Size | Who still references it | Verdict |
|---|---|---|---|
| `archive/kernel/native-abi`, `archive/tests/native-abi` | 660 KiB | `docs/kernel/roadmap.md` (M121 prose) only | **Delete.** The native ABI is gone and will not come back; M121's entry can say so without a copy of the code. |
| `userspace/src/mojo_core.c` + `include/mojo*.h` | 20 KiB | `userspace/Makefile`, only to build `m57_smoke` | **Delete with its test** if `m57_smoke` is Chromium-IPC scaffolding whose subject is now covered by the Debian lane; keep only if that smoke proves a syscall path nothing else reaches. Decide by reading the test, not the name. |
| `userspace/compat/crypt.c`, `utmp.c` | 16 KiB | nothing found outside the directory | **Delete** — musl and the distribution provide both. Verify no link error appears in the PAM and login paths. |
| `userspace/libcxx_compat.c` | — | `userspace/Makefile` (`BESPOKE_LIBCXX_EXTRA`) | **Audit.** libc++ comes from Alpine since M89/M121; if the bespoke libc++ is gone, this object and its variable go with it. |
| `userspace/hello_native.c` | — | `userspace/Makefile`, copied into the rootfs | **Delete** unless a lane asserts on it; a hello-world in `/home` is not a test. |
| `tools/blobs/*.bin`, `*.S`, `hello_b1nix.rs` | 14 files | `Makefile`, `tests/smoke.sh`, `tools/configs/static-allowlist.txt` | **Keep, but rebuild from source.** These prove a foreign Linux binary runs unmodified — exactly the property the distribution rests on. Committed `.bin` files are the problem, not the tests: build them in the lane and stop tracking the binaries. |
| `tests/liveusb.sh` | — | nothing; not wired into `smoke.sh` or the Makefile | **Delete at phase D**, when the live ISO lane replaces it. Until then it is the only recipe for a bootable stick; read it for anything worth carrying over first. |
| `userspace/include/` | 179 files | `Makefile` dependency lists, `tools/toolchain/build-toolchain.sh` (copies a subset over musl's headers) | **Shrink, do not delete.** Go header by header: anything musl already provides is removed from the copy list and then from the tree. The subset that stays is whatever declares b1nix-only interfaces (`include/b1nix/`). |
| `userspace/bin/smoke/` | 58 tests | `userspace/Makefile`, `tests/smoke.sh` | **Shrink by the rule below.** |
| `userspace/rootfs-overlay/` | 40 files | `Makefile`, `userspace/Makefile` | **Audit against the Debian image.** The overlay configures the Alpine smoke image; anything it sets that the distribution sets differently is a source of "works in smoke, broken in the product". |
| `tools/configs/openrc/`, `applet-manifest.conf` | 5 files | `Makefile`, `tools/images/00-smoke.start`, the Alpine fetch scripts | **Keep.** The Alpine lane stays as the fast CI path; this is its configuration. |
| `tools/selfhost/` | 2 files | `Makefile` | **Keep.** The self-host build is a kernel test on release tags. |

## Shrinking the smoke binaries

The Alpine lane stays, so the musl smoke binaries stay too. What changes is
the standard for keeping one:

- **Delete a test whose subject is exercised harder by the distribution.**
  If systemd, apt, Calamares or Plasma runs the same kernel path every boot and
  a lane asserts on the result, a hand-written probe adds maintenance and no
  coverage.
- **Keep a test that isolates a syscall or an error path.** A distribution
  proves the happy path; it rarely proves `EINVAL` on a bad argument, a race
  under four threads, or a boundary the kernel must reject. That is what the
  own binaries are for, and those get more valuable as the userspace above
  grows.
- **Keep every regression test.** A test written to catch a specific bug is
  never redundant, whatever else covers the area.

When a test goes, its marker goes from `tests/smoke.sh` in the same commit, and
the check count in any lane summary is updated. A lane that silently loses
checks is the same failure mode as a fake pass.

## Order of work

Cleanup is filler work between phases, not a phase. The two pieces worth doing
before phase A are the ones that would otherwise be packaged by accident:
`archive/`, and whatever the rootfs overlay sets that the distribution must
not inherit.
