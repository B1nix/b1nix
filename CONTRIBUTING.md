# Contributing

## Sign your work

Patches carry a `Signed-off-by:` line certifying the
[Developer Certificate of Origin](https://developercertificate.org/). There is
no CLA.

## Before you send a patch

- Build both architectures with no new warnings.
- Run the smoke suite in the foreground and say in the pull request which lanes
  you ran and what they printed.
- New feature, new checks: a feature without a real smoke check is not
  finished.

## No fake passes

This is the rule the project is strictest about. A test marker is printed only
when the operation really succeeded and was verified. Never print a marker
unconditionally, swallow an error, add a test-mode shortcut, or mark a feature
unsupported to make a lane green. If a lane fails, the implementation is what
gets fixed.

## Commits

- One subject line, at most 60 characters, starting with a type: `fix:`,
  `feat:`, `clean:`, `refactor:`, `perf:`, `docs:`, `test:`, `build:`. An
  optional scope, like `fix(lkpi):`.
- A body only when the "why" is not obvious from the subject: one to three
  lines, no history, no lists.

## Imported code

Linux source imported through linuxkpi — filesystems, DRM, drivers — is not
patched. If imported code does not work, the shim is wrong. A patch to imported
source will be sent back with that reason.

## Where things live

`docs/README.md` indexes the documentation: `docs/kernel/` for the kernel,
`docs/distro/` for the distribution, `docs/versioning.md` for the version
numbers.
