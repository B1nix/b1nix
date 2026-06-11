# GNU bash 5.2 port

b1nix ships **GNU bash 5.2.37** as the default interactive shell (the "default
terminal"). bash is cross-built against the b1nix userspace ABI by
[`tools/build-bash.sh`](../tools/build-bash.sh) and embedded into the initramfs
as `/bin/bash`. BusyBox `ash` remains `/bin/sh` for `#!/bin/sh` system scripts;
bash is the login shell (`/etc/passwd`) and the console shell `/bin/init`
launches.

## Build

`tools/build-bash.sh` follows the autotools cross-build pattern of
`build-dropbear.sh`/`build-curl.sh` (the `b1nix-autotools-cc` wrapper compiles +
links against `libb1nix.a`). bash cannot run target binaries during `configure`,
so a **preseeded `config.cache`** answers the run-time feature probes
(job control, FIFOs, `/dev/fd`, `sigsetjmp`, POSIX signals, bundled termcap).

Key flags / patches:

- `--without-bash-malloc` — use libb1nix `malloc`, not bash's sbrk allocator.
- `--disable-nls`, bundled readline + history + termcap.
- `-D_POSIX_VERSION=200809L` (build-scoped, **not** in the global libc headers —
  it flips OpenSSL's secure-memory path to `mlock`/`madvise` we don't provide).
  bash needs it so `posixwait.h` picks `int` over the BSD `union wait`.
- `-fcommon` — termcap/readline's `BC`/`PC`/`UP` are tentative (common)
  definitions that modern clang's `-fno-common` would clash.
- `--allow-multiple-definition` (via `B1NIX_LD_EXTRA`) — bash ships its own
  `getenv`/`setenv`/`putenv`/`unsetenv` that intentionally shadow libb1nix's;
  they appear first in the link line, so the linker keeps bash's set.
- A one-line source guard in `parse.y`/`y.tab.c`: upstream references
  `shell_input_line_property[]` (declared only under `HANDLE_MULTIBYTE`) inside
  an `ALIAS||DPAREN` block. b1nix's libc lacks the wide-char functions, so
  `configure` leaves `HANDLE_MULTIBYTE` off; the patch guards that one statement.
- `llvm-strip -S` to shrink the embedded payload (~3 MB → ~1 MB).

## libc additions this port required

- `sigsetjmp`/`siglongjmp` + `sigjmp_buf` (`setjmp.h`, `stdlib.c`) — `sigsetjmp`
  is a macro so the real (assembly) `setjmp` captures the *caller's* frame.
- `setgrent`/`getgrent` over `/etc/group` (`grp.c`) — group-name tab completion.
- `setlinebuf` (`stdio.h`), `ffs` (`strings.h`), and `<signal.h>` pulled into
  `<sys/select.h>` so `sigset_t` is visible where readline reaches for it.

## Limitations / future work

- **No multibyte/UTF-8** (`HAVE_MULTIBYTE` off): the libc lacks `mbrtowc`,
  `wcwidth`, `mbsrtowcs`, etc. Line editing and string ops are byte-oriented.
  Implementing the wide-char functions would let bash enable UTF-8.
- `/bin/sh` stays BusyBox `ash`; bash is not (yet) the POSIX `sh`.

## Verification

`BASH-SMOKE` in the boot self-test runs `/etc/bash-smoke.sh` under `/bin/bash`
and checks bash-only syntax ash does not implement: `BASH_VERSION`, indexed
arrays, `[[ ]]` glob + `=~` regex, `$(( ))`, `{a..b}` brace ranges, C-style
`for`, `${var//x/y}` substitution, and function `local`s. Verified green on the
i686 smoke suite (single-CPU and `-smp 4`); the x86_64 binary builds with the
same recipe.
