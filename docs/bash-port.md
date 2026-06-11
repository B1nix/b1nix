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
calls). UTF-8 is the libc-wide default: `MB_CUR_MAX` is 4 in `<stdlib.h>` and
the non-restartable conversions (`mbtowc`/`wctomb`/`mbstowcs`/`wcstombs`)
delegate to the same UTF-8 primitives, so every port sees one encoding.

## bash as the login shell (and the dropbear saga)

`/etc/passwd` sets `/bin/bash` for root and user, so SSH logins and `login`
get bash too. Making that work surfaced three layered problems, none of them
actually bash's fault:

1. **`/etc/shells` did not exist.** dropbear validates the passwd shell with
   `getusershell()`; its bundled fallback list is `{"/bin/sh","/bin/csh"}`, so
   a bash login was refused outright ("User 'root' has invalid shell,
   rejected"). The initramfs now ships `/etc/shells` listing `/bin/sh`,
   `/bin/bash`, `/bin/ash`, and the BusyBox path.
2. **A real libc bug in `fgets`.** dropbear's `initshells()` parses
   `/etc/shells` with `while (fgets(cp, flen - (cp - strings), fp))` over a
   shrinking buffer. Our `fgets` returned non-NULL without reading anything
   when `size <= 1`, so once the buffer was exhausted the loop spun forever —
   the SSH session hung *during auth* for every login. `fgets` now returns
   NULL for `size <= 0` and handles `size == 1` with a real EOF probe
   (`fgetc`/`ungetc`).
3. **A stale-object build trap.** dropbear's own Makefile does not track our
   libc headers, so a top-level rebuild after the `fgets` fix only *relinked*
   old objects — the fix was not in the binary even though every mtime said
   otherwise. (`rm *.o` in the dropbear tree before rebuilding was required;
   the same applies to any header-level libc fix that must reach a port.)

## Limitations / future work

- `/bin/sh` stays BusyBox `ash`; bash is not (yet) the POSIX `sh`.

## Verification

`BASH-SMOKE` in the boot self-test runs `/etc/bash-smoke.sh` under `/bin/bash`
and checks bash-only syntax ash does not implement: `BASH_VERSION`, indexed
arrays, `[[ ]]` glob + `=~` regex, `$(( ))`, `{a..b}` brace ranges, C-style
`for`, `${var//x/y}` substitution, function `local`s, and UTF-8 awareness
(`${#var}` counts characters and `${var:1:1}` is character-indexed). Verified
green on i686 and x86_64, single-CPU and `-smp 4`.
