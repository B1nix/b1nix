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

## UTF-8 / multibyte

bash is built with `HANDLE_MULTIBYTE` on, so `${#var}`, `${var:off:len}`,
case conversion, and readline line editing are UTF-8 character-aware (not
byte-oriented). This needed a UTF-8 wide-character module in the libc
(`userspace/libc/wchar.c`): `mbrtowc`/`mbrlen`/`mblen`/`wcrtomb`/`mbsinit`,
`wcwidth`/`wcswidth`, `mbsrtowcs`/`wcsrtombs`, `btowc`/`wctob`, and the wide
string helpers (`wcscmp`/`wcscoll`/`wcschr`/`wcsdup`/…), plus a `mbstate_t`
type in `<wchar.h>`. Conversion is UTF-8-only and stateless (an incomplete
trailing sequence is reported as `(size_t)-2` rather than carried across
calls). Two knobs are build-scoped so other ports stay byte-oriented:
`MB_CUR_MAX` (defaulted to 1 in the libc, overridden to 4 for the bash build)
and the conversion functions themselves are detected by bash's `configure`.

## Limitations / future work

- `/bin/sh` stays BusyBox `ash`; bash is not (yet) the POSIX `sh`.
- The libc default `MB_CUR_MAX` is still 1; only bash opts into UTF-8. A global
  switch would let every port handle multibyte text.

## Verification

`BASH-SMOKE` in the boot self-test runs `/etc/bash-smoke.sh` under `/bin/bash`
and checks bash-only syntax ash does not implement: `BASH_VERSION`, indexed
arrays, `[[ ]]` glob + `=~` regex, `$(( ))`, `{a..b}` brace ranges, C-style
`for`, `${var//x/y}` substitution, function `local`s, and UTF-8 awareness
(`${#var}` counts characters and `${var:1:1}` is character-indexed). Verified
green on i686 and x86_64, single-CPU and `-smp 4`.
