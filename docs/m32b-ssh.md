# M32b — SSH Daemon Prerequisites

This milestone brings up everything b1nix needs to run a real SSH **daemon**
on localhost: a self-contained crypto/RNG stack, persistent host keys, a working
pseudo-terminal substrate, a hardened socket API, login/session plumbing, auth
storage and policy, and a service lifecycle — then ports an actual sshd and
smoke-tests an end-to-end login over loopback.

The work is split so that **one roadmap item == one commit**. The kernel-side
prerequisites (sockets, PTY, sessions) land first because they are independently
smoke-testable and useful well beyond SSH; the daemon port and its auth/lifecycle
wiring land on top of them.

## Daemon choice: Dropbear

The roadmap allows `dropbear` or `tinyssh` for the first daemon-sized port and
defers OpenSSH until PTY, privilege separation, and broader POSIX coverage are
stronger. **We pick Dropbear.**

| Criterion | Dropbear | tinyssh | OpenSSH |
|---|---|---|---|
| Bundled crypto (no external lib required) | yes (libtomcrypt + libtommath, in-tree) | yes (tweetnacl) | no (needs OpenSSL/libcrypto) |
| Password auth (`/etc/shadow`) | yes | **no** (pubkey only) | yes |
| Built-in TCP listener | yes (own `accept()` loop) | **no** (needs `tcpserver`/inetd/socket-activation) | yes |
| PTY / interactive shell allocation | yes (own `pty.c`) | minimal | yes |
| Build system | autotools `configure` (matches our existing port pattern) | hand `Makefile` | autotools, heavy |
| Footprint / portability for a freestanding libc | small, embedded-targeted (uClibc/musl-friendly) | tiny | large, many POSIX assumptions |

Rationale:

* **Self-contained crypto.** Dropbear ships libtomcrypt + libtommath in-tree, so
  the M32b "crypto baseline" item (Curve25519/DH, Ed25519/ECDSA/RSA,
  chacha20-poly1305, AES-CTR/GCM, HMAC-SHA2, SHA2, constant-time compare) is
  satisfied by the daemon's own vetted code rather than a new hand-rolled stack.
  b1nix only has to provide a good `getrandom(2)` to seed it — which already
  exists (`SYS_GETRANDOM`, gated `rdrand` + software fallback).
* **Password auth + own listener.** Dropbear authenticates against
  `/etc/passwd`/`/etc/shadow` and runs its own `listen()`/`accept()` loop, which
  matches b1nix's existing auth storage and socket layer. tinyssh is pubkey-only
  and depends on an external `tcpserver`, which would mean porting ucspi-tcp too.
* **PTY shells.** Dropbear allocates a pty for interactive logins, exercising the
  exact `/dev/ptmx` substrate this milestone builds.

OpenSSH stays deferred: it needs OpenSSL, privilege-separation `chroot`+a
dedicated unprivileged uid, and a much larger POSIX surface than dropbear.

## Build plan

Dropbear is built like the other upstream ports (`tools/build-curl.sh`,
`tools/build-openssl.sh`): fetch the release tarball, patch `config.sub` to
accept the `x86_64-b1nix` host triplet, run `./configure --host=x86_64-b1nix
--disable-shared --enable-static` through `tools/b1nix-autotools-cc`, build the
static `dropbearmulti` binary (server + `dropbearkey` + client), and embed it in
the initramfs as an `xxd -i` `.inc`. Bundled libtomcrypt/libtommath are used
(`--disable-openssl` equivalent: dropbear uses its in-tree crypto by default).

Dropbear is configured for the b1nix environment via `localoptions.h`:

* enable password auth, disable syslog/wtmp/lastlog/PAM (none on b1nix),
* seed the PRNG from `getrandom(2)` / `/dev/urandom`,
* loopback-friendly defaults for early smoke testing.

## Item map (one commit each)

1. **Pick sshd target** — this document. *(done)*
2. **Crypto/RNG baseline** — dropbear's bundled libtomcrypt/libtommath build for
   b1nix; `getrandom(2)` seeds it; primitives proven by smoke.
3. **Host-key storage** — persistent `/etc/ssh/`, first-boot key generation via
   `dropbearkey`, permission checks, inspect/regenerate tooling.
4. **PTY/TTY substrate** — `/dev/ptmx`, `/dev/pts/N`, `openpty`/`grantpt`/
   `unlockpt`/`ptsname`, `TIOCSCTTY`, `TIOCGWINSZ`/`TIOCSWINSZ`, per-task
   controlling terminal, SIGHUP hangup.
5. **Socket API hardening** — `setsockopt`/`getsockopt` (`SO_REUSEADDR`,
   `SO_KEEPALIVE`, `TCP_NODELAY`, `SO_ERROR`, buffers), `getsockname`/
   `getpeername`, `shutdown`, listen backlog, nonblocking edge cases.
6. **Login/session plumbing** — controlling-terminal handoff on the session
   leader; `fork`/`exec`/`setuid`/`setgid`/`setsid`/`chdir`/env for login shells.
7. **Auth storage and policy** — `/etc/shadow` password auth in sshd, optional
   `authorized_keys`, account shell/home validation, login-failure accounting,
   root-login defaults.
8. **Service lifecycle** — init/service entry for sshd, background daemon mode,
   log output, pid tracking, clean shutdown/restart, loopback-only early bind.
9. **Localhost smoke** — SSH handshake, host-key persistence, password login to
   run one command, interactive shell over pty, negative auth cases.

See [`docs/roadmap.md`](roadmap.md) M32b for live status.
