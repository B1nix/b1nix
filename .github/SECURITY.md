# Security

b1nix is an experimental operating system: an original kernel running a Debian
userspace. Treat it as such. It has had no security audit, its kernel is young,
and it should not hold data you cannot lose.

## Reporting a vulnerability

Email the project address listed on the website. Please include:

- the release or kernel version (`uname -r`, `/etc/os-release`),
- what the issue allows an attacker to do,
- the smallest reproducer you have.

Expect an acknowledgement within a week. We aim to fix and release within 90
days of a report, and to credit reporters who want it.

Please do not open a public issue for a vulnerability before that window has
run, unless the issue is already public elsewhere.

## Scope

- **In scope**: the b1nix kernel, the packages in the b1nix overlay, the
  release and signing process, and the installer configuration we ship.
- **Out of scope**: vulnerabilities in Debian packages we redistribute
  unmodified — report those to Debian, and they reach b1nix users through
  `apt` without us. Bugs that require physical access to an unlocked machine,
  and the absence of Secure Boot, are known and documented, not vulnerabilities.

## What we do not promise

No LTS branch, no backports, no CVE assignment. A security fix ships as a point
release, and the release notes say what it fixes.
