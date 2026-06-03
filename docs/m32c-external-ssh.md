# M32c — External SSH Access & Operator Networking

M32b proved the SSH daemon end to end **over loopback** (Dropbear handshake,
password auth vs `/etc/shadow`, PTY login shells, service lifecycle). M32c makes
the same daemon reachable **from outside the guest**: you can now SSH straight
into a running b1nix VM from the host.

```sh
# Boot a VM with the SSH port forwarded and log in from the host:
sh tests/ssh-hostfwd.sh x86         # automated end-to-end proof
# …or by hand (see "Running it by hand" below):
ssh -p 2222 root@127.0.0.1          # password: root
```

## What landed

### 1. Networking is on by default

Previously DHCP only ran in test mode or with an explicit `b1nix.net=dhcp`.
Now a NIC is brought up automatically on every boot:

- `kernel/net/net.c` `net_init()` calls `dhcp_init()` whenever a NIC is present,
  unless `b1nix.net=off` (or `b1nix.nonet`) is on the kernel command line.
- QEMU user-mode networking assigns `10.0.2.15` (gateway `10.0.2.2`); a
  deterministic static fallback (`10.0.2.15`) kicks in if DHCP times out.
- `b1nix.net=dhcp` is still accepted as an explicit no-op for back-compat.

### 2. Explicit, safe sshd bind policy

`/etc/init.d/sshd` (embedded in `kernel/fs/initramfs.c`) now chooses its bind
address from the kernel command line. **Loopback-only is the safe default** so
the daemon is never exposed by accident:

| Kernel cmdline knob       | Bind address    | Use                                   |
|---------------------------|-----------------|---------------------------------------|
| *(none)*                  | `127.0.0.1:22`  | safe default — local only             |
| `b1nix.ssh-loopback`      | `127.0.0.1:22`  | explicit loopback (smoke harness)     |
| `b1nix.ssh-external`      | `0.0.0.0:22`    | deliberate opt-in: all interfaces     |

> Implementation note: the knobs are matched with `grep -q … /proc/cmdline`
> inside the `start)` case arm. Keep shell `#` comments **out** of that arm —
> comment text containing `)` collides with `case` pattern syntax in the
> in-kernel shell and silently aborts the arm (this bit us during bring-up).

### 3. Inbound TCP over the real NIC

A host OpenSSH client connecting through QEMU `hostfwd` exercises the kernel's
**passive-open** TCP path end to end — distinct from the `127.0.0.1` loopback
fast path:

- `tcp_listen()` registers a `TCP_LISTEN` conn keyed on the local port only
  (wildcard remote), so a socket bound to `0.0.0.0` accepts any inbound peer.
- `tcp_input()` matches an inbound SYN to the listener, creates a
  `TCP_SYN_RECEIVED` conn, emits the SYN-ACK, and promotes it to
  `TCP_ESTABLISHED` on the client's ACK.
- `ipv4_receive()` accepts the segment because its destination matches the
  DHCP-assigned IP (`net_get_ip()`); the SYN-ACK is routed to the gateway via
  ARP. The loopback special-casing in `ipv4_send()` does not touch this path.

The host's OpenSSH completing a full curve25519 KEX + ed25519 host-key verify +
chacha20-poly1305 channel + password auth + remote-exec is the proof that all of
the above works for unsolicited inbound connections.

### 4. Hardened external-login defaults

Tightened in `/etc/init.d/sshd` before treating SSH as an exposed service:

- **Connection lifecycle (always on):** `-I 300` (idle timeout, 5 min),
  `-K 60` (keepalive), `-T 6` (max auth tries).
- **Root / password policy (opt-in,** so the loopback smoke's root+password
  login keeps working): `b1nix.ssh-no-root` → `-w` (no root login),
  `b1nix.ssh-pubkey-only` → `-s` (disable password auth, pubkey only).
- **Host-key persistence:** the Ed25519 host key prefers `/persist/etc/ssh`
  when the persistent root image is mounted, so it survives reboots; it falls
  back to the volatile initramfs `/etc/ssh` otherwise (e.g. the smoke harness).
- **Logging:** Dropbear stdout/stderr go to `/var/log/sshd.log`; the pid is
  tracked in `/var/run/sshd.pid`.
- **Login home dirs:** `/etc/rc` creates `/root` and `/home/user` so a login
  shell (local or over SSH) has a valid working directory.

## Running it by hand

```sh
# Build a normal-mode ISO (no in-kernel test suite), bind SSH to all interfaces:
make ARCH=x86 KERNEL_CMDLINE="b1nix.ssh-external=1" iso

# Boot with the SSH port forwarded to the host's 2222 (restrict=off so the
# host can reach the listener; QEMU still serves DHCP either way):
qemu-system-x86_64 \
  -cdrom build/x86/b1nix.iso -serial stdio -display none \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=net0

# From the host, once you see "sshd: started":
ssh -p 2222 root@127.0.0.1            # password: root  (or user / user)
```

## Automated test — `tests/ssh-hostfwd.sh`

Deliberately separate from `tests/smoke.sh` so the default CI run stays
deterministic and never opens a forwarded port. It:

1. Builds a normal-mode ISO with `b1nix.ssh-external=1` (networking is default).
2. Boots QEMU in the background with `hostfwd=tcp:127.0.0.1:2222-:22`, serial to
   a log file.
3. Waits for `net: dhcp bound to …` **and** `sshd: started`.
4. Probes inbound TCP for the `SSH-2.0-dropbear_…` banner (sends a client ident
   first — Dropbear completes its half of the ident exchange before a bare
   half-close from the probe would abort it).
5. Logs in from the host with OpenSSH (password auth driven by `expect`) and
   runs `id; uname -a; echo EXTSSH-$((6*7))-OK`. Success requires the
   **guest-computed** `EXTSSH-42-OK` in the transcript (the literal command text
   echoed by `expect`'s `spawn` still shows `$((6*7))`, so the marker can only
   appear if the remote shell actually ran).

Host requirements: `qemu-system-x86_64`, `ssh` (OpenSSH client), `expect`.

```
=== B1NIX External SSH Smoke (x86, hostfwd 2222->22) ===
  PASS kernel/ISO builds
  PASS networking up (DHCP)
  PASS sshd started
  PASS inbound TCP + SSH banner (SSH-2.0-dropbear_2022.83)
  PASS host-to-guest SSH login + remote command (root@guest via 2222)
=== M32C-EXTSSH: host-to-guest SSH OK ===
```

## Status

| Item                                   | Status     |
|----------------------------------------|------------|
| QEMU host-to-guest SSH smoke path      | `done`     |
| Explicit sshd bind policy              | `done`     |
| Inbound TCP exposure on a real link    | `done`     |
| Hardened external-login defaults       | `done`     |
| Bare-metal SSH reachability (LAN)      | `deferred` (gated on M37 real-HW NIC coverage) |

See also [`docs/m32b-ssh.md`](m32b-ssh.md) for the loopback SSH bring-up and the
kernel bugs fixed there (socket/PTY teardown-on-close, loopback TCP re-entrancy,
`select()` POLLHUP mapping).
