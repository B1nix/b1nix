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

## Status — loopback handshake (2026-06-02)

Bringing up the end-to-end loopback handshake (item 9) surfaced and fixed two
real kernel bugs that also harden the whole networking stack:

1. **Sockets shared across `fork()` were torn down by a sibling's `close()`.**
   `vfs_close()` invoked the socket's `->close` op (TCP FIN + free of the shared
   `vfs_socket_state`) on *every* fd close, ignoring the handle refcount. A
   dropbear `accept()`+`fork()` server, where the parent `close()`s its copy of
   the connection fd while the child keeps serving, therefore FIN'd the peer
   mid-handshake (client saw "Remote closed") and freed the state out from under
   the child (later `getsockname`/`getpeername` returned garbage). Fix: the
   teardown moved to the `->release` op, which only runs when the refcount
   reaches zero (`kernel/net/socket.c`). The accepted socket also now copies the
   listener's bound address into its `local` so `getsockname()` on the
   connection returns a valid family.

2. **Loopback TCP delivered synchronously, re-entering the TCP state machine.**
   `ipv4_send()` for a `127.0.0.0/8` destination called `ipv4_receive()` inline,
   so `tcp_send → ipv4_send → ipv4_receive → tcp_input → tcp_send → …` recursed
   within a single send and corrupted/wedged multi-packet exchanges. Fix:
   loopback datagrams are now enqueued (`net_loopback_enqueue`) and drained in a
   clean context by `net_poll()`/`net_task` (`kernel/net/net.c`, `ipv4.c`,
   `net.h`). This made every loopback TCP smoke test pass (tcp-echo, http-get,
   wget-loopback, …) and removed a suite hang.

With both fixes the localhost SSH exchange now runs the full key exchange:
banner exchange, KEXINIT algorithm negotiation (ssh-ed25519 host key,
chacha20-poly1305 cipher), and the curve25519 ECDH packet exchange in both
directions all complete and deliver in order.

**UPDATE (2026-06-02):** The loopback SSH handshake hang/failure has been resolved. The client (`dbclient`) successfully receives `KEX_ECDH_REPLY`, runs `recv_msg_kexdh_reply` to finish KEX, sends `NEWKEYS`, performs password authentication (verifying against `/etc/shadow`), and runs the remote command (`echo M32B-SSH-LOGIN-OK`), which outputs to the client and prints `M32B-SSH: ok handshake`. This was resolved by fixing `SYS_SELECT` FD mapping and updating `socket_poll` to return `B1NIX_POLLHUP` and control `B1NIX_POLLOUT` based on connection state transitions (e.g. `TCP_CLOSE_WAIT`), avoiding select-loop starvation on single-CPU. `dropbearkey` host-key generation and the crypto baseline are green.

Auth groundwork landed alongside: libc `getpwnam`/`getpwuid` now substitutes the
real `/etc/shadow` hash when `/etc/passwd` records the `x` marker
(`userspace/libc/pwd.c`), and a POSIX `getpass()` was added — both prerequisites
for dropbear's `/etc/shadow` password auth (item 7).

## Status — item-9 negative-auth and interactive-PTY coverage (2026-06-02)

The localhost smoke now drives **three** logins against one dropbear instance
(fork-per-connection), each emitting its own `M32B-SSH` marker:

1. `ok handshake` — positive password login + a remote command (the path above).
2. `ok negauth` — a **wrong** password is rejected by the daemon: it retries to
   the server's auth-try limit, the server disconnects, the client exits, and
   the remote command never runs (verified: the `SHOULD-NOT-RUN` marker is
   absent from the captured output).
3. `ok pty` — an **interactive shell over a remote PTY**: `dbclient -t` forces a
   remote pseudo-terminal, so sshd allocates a pty and spawns the login shell
   (`/bin/sh`) on the slave; the shell runs a command that writes a marker file,
   checked over the shared VFS (not stdout, so the pty ECHO of the typed line
   cannot cause a false pass). dbclient also needs its *own* stdin to be a tty
   (it sets the local terminal raw), so the test hands it a local pty slave via
   `login_tty`.

Bringing up the PTY path surfaced and fixed a **third real kernel bug** of the
same class as the earlier socket/loopback fixes: `kernel/dev/pty.c` did its pty
teardown (which nulls `private_data`) from a per-fd `.close` op, and `vfs_close`
invokes `.close` on *every* descriptor close. `login_tty` dups the slave onto
fd 0/1/2 and then closes the original slave fd — that close tore down the shared
pty, so the next `tcgetattr` saw `private_data == NULL` and returned `EINVAL`,
and dbclient aborted with "Failed to set raw TTY mode" before it ever sent the
command. Fix: teardown (and the master-close `SIGHUP` hangup) now runs **only**
from the refcount-zero `.release` op; `.close` was removed from both pty op
tables. The in-process PTY tests (`M32B-PTY: ok …`, including `hangup`) still
pass.

`MAX_TCP_CONNS` was also raised 16 → 32 (`kernel/net/tcp.c`): three back-to-back
SSH logins plus a SIGKILLed client leave several connections occupying the conn
table (some stuck, some in TIME_WAIT), and the white-box kernel TCP tests that
run immediately afterwards could no longer allocate a connection (`tcp_accept`
returning NULL). b1nix now runs a fork-per-connection SSH daemon, so a larger
table is warranted.

**Single-CPU: all green** — `handshake` + `negauth` + `pty` + the kernel TCP
`window-throttle` test all pass and the suite reaches `B1NIX-TEST: done`.

**SMP caveat (pre-existing).** Under `-smp 4` the full suite hangs
intermittently (no `B1NIX-TEST: done`; the stall point varies — loopback TCP,
the M11 pipe tests, …). This is **not** introduced by the SSH work: stashing all
in-progress changes and running the committed branch tip under `-smp 4` hangs on
roughly one run in three as well, so a latent race lives in the committed
loopback-deferral / `net_task` path. The extra loopback traffic from three SSH
logins simply raises the hit rate. Tracked separately from the SSH items.
