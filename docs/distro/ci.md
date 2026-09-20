# Continuous integration and the release pipeline

What runs in the cloud, what only runs here, and why the split is not where a
normal project would put it.

## The constraint that decides everything

This project's tests boot QEMU. A hosted runner's speed for that is not a
detail — it is the difference between a suite that runs on every tag and one
that cannot finish inside a job's time limit.

**Before designing around it, measure it.** The first CI job does exactly one
useful thing: print `ls -l /dev/kvm`, `nproc`, `free -m`, and the wall time of
one short smoke lane. Everything below branches on that result, and the
measured numbers go into this file once they exist.

- **If KVM is available and fast enough**: the quick kernel lanes run in CI on
  every push, and the distribution lanes run in CI on tags.
- **If it is not**: CI builds, packages, signs and publishes, and every lane
  runs here, with the release checklist as the record that they did. This is
  the assumption the plan is written against, because it is the one that still
  works if the answer is bad. A self-hosted runner on this machine is the
  upgrade path, and it is worth setting up only once the manual runs become the
  bottleneck.

Either way, `INSTALL-SMOKE` — booting an ISO, running Calamares, installing to
a virtual disk and booting the result — is the lane most likely to exceed a
hosted job, and the first one to move locally.

## What CI does on a tag

1. Builds the kernel for both architectures from the tag, with ccache warm
   where the cache survives.
2. Builds every overlay package with `tools/packages/build-deb.sh`, runs
   `lintian`, fails on errors.
3. Builds the netinstall image for both architectures.
4. Writes the build manifest: every Debian package version that went in, the
   kernel commit, the toolchain versions.
5. Signs — see below.
6. Publishes: images and `SHA256SUMS` to Releases, the apt tree to Pages,
   `b1nix-kernel-dbg` to Releases as well.
7. Fails loudly if the version it is about to publish sorts below what is
   already in the suite.

## Keys

- **The signing key never reaches CI.** The primary key is offline. What CI may
  hold is a signing subkey whose scope is exactly "sign `Release` and
  `SHA256SUMS` for this repository", stored as a repository secret, revocable
  without touching the primary.
- If that is still too much, the unsigned artifacts are downloaded and signed
  here, then re-uploaded, and the release is not announced until that is done.
  Slower, and perfectly acceptable at this scale.
- Every secret is used by exactly one workflow step, never echoed, and the
  workflow files are reviewed as carefully as kernel code — a compromised
  release is worse than a compromised build.

## What CI must never do

- **Never publish a release that no lane covered.** A green build is not a
  green release; the checklist is what ships something.
- **Never retry a flaky lane until it passes.** A lane that needs retries is a
  bug report, and the retry hides which one.
- **Never skip a step because the job is slow.** A step too slow for the job is
  moved out of the job, visibly, in this file.

## Cheap things worth having early

- A job that only builds both kernels on every push. It is fast, it catches the
  warning-free rule, and it costs nothing.
- A link checker over `docs/`, because half of the documentation is now
  cross-references.
- A job that verifies the published repo from outside: in a clean Debian
  container, add the repo, `apt update`, `apt install b1nix-kernel`, `dpkg -L`.
  This catches the class of mistake that is invisible on the build host.
