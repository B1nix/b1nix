# Distribution smoke lanes

The kernel's lane rules are in
[../kernel/build-conventions.md](../kernel/build-conventions.md) and apply
unchanged: a lane states its identity, no wait is bounded in wall-clock time,
and a per-file loop is skippable. This file adds what the distribution lanes
need on top, so that the first one written does not invent a style the next
three copy.

The distribution lanes are slower and heavier than the kernel ones. They boot a
real Debian, install packages and sometimes write to disks. That makes two
things more important than in the kernel suite: they must say clearly *which
stage* failed, and they must never pass by accident.

## The lanes

| Lane | What it proves | Phase |
|---|---|---|
| `PKG-SMOKE` | the overlay packages build, publish and install into a clean Debian | A |
| `DISTRO-SMOKE` | the installed image boots to a systemd target, apt works, the two-kernel fallback works | B |
| `INSTALL-SMOKE` | Calamares installs onto a blank disk, and the installed disk boots | D |
| `DESKTOP-SMOKE` | login, browser, sound, and suspend once it exists | D |
| `UPGRADE-SMOKE` | release N upgrades to N+1 through apt and still boots | G |

## Naming and markers

- The marker prefix is the lane name: `DISTRO-SMOKE: ok <check>`, and
  `DISTRO-SMOKE: FAIL <check> — <what was expected>`. One check, one line.
- A check name is a noun phrase that survives out of context:
  `apt-update-overlay`, `fallback-after-three-tries`, `installed-disk-boots`.
  Not `test3`, not `step-2`.
- Each lane sets `SMOKE_LANE` explicitly. Several of these lanes boot an image
  another lane built, so the default from `B1NIX_ISO_NAME` is wrong for them by
  construction.
- Images live in `build/$ARCH/`, logs and every other artifact in
  `smoke_run/`. Nothing is written to the source tree.

## Stages, and telling them apart

A distribution lane has four failure modes and must distinguish them, because
they need different people and different fixes:

1. **Build failed** — the image or package was never produced. The lane exits
   with a distinct code and says so; it does not report missing markers.
2. **Boot failed** — the image exists but produced no login. This is a kernel
   bug until proven otherwise.
3. **Stage failed** — the system booted and a step (apt, install, upgrade) did
   not complete. The lane names the step and copies the relevant log into
   `smoke_run/`.
4. **Check failed** — everything ran, an assertion is false.

A lane that prints "missing marker" for all four is useless at the hour a
release is being cut.

## The known-degraded set

`systemctl is-system-running` returns `degraded` on a system with any failed
unit, and on a young kernel there will be some. The lane does not accept
`degraded` blindly and does not demand `running` either. It compares the failed
units against a list checked into the tree, one unit per line with a reason:

```
# tools/configs/known-degraded.txt
systemd-networkd-wait-online.service   no carrier in the smoke topology
```

- A unit not on the list fails the lane.
- A unit on the list that *stopped* failing is also a failure: the line is
  stale and must be removed, or the list slowly becomes a place where real
  breakage hides.
- Every line needs a reason. A list of unit names with no reasons is a way of
  forgetting.

## Timeouts

The distribution lanes are the slowest thing in the suite, so their timeouts
are generous — and, per the kernel rule, they are not wall-clock. An install
that is progressing must not be killed because the host is busy; an install
that has stopped producing output is killed quickly. The signal is output, not
elapsed time.

## What a lane may never do

Restating the project rule in the terms these lanes will be tempted to break:

- **No marker without the operation.** Printing `ok installed-disk-boots`
  because the installer exited 0 is a fake pass: the assertion is that the disk
  boots, so the lane boots it.
- **No stage skipped on a slow host.** If a step is too slow to run every time,
  it gets its own lane, not a conditional.
- **No network-dependent check without a network assertion first.** A failure
  to reach `deb.debian.org` is reported as exactly that, never as "apt broken".
- **No reuse of a stale image.** `SKIP_BUILD` exists for iteration and has
  already produced one wrong result in this tree; a release run never uses it,
  and the lane prints the image's build time so a stale one is visible in the
  log.
