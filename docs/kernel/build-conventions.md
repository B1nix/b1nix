# Build and smoke conventions

Three rules that keep the build fast and the smoke results honest.

## A per-file tool loop must be skippable

A recipe that runs `readelf`, `nm` or a similar tool once per file over
hundreds of files dominates the time of a build that has nothing to do. Guard
such a loop with a stamp file under `$(BUILD_DIR)`, so that wiping the build
tree redoes the work. Skip the loop only when the destination still exists and
the newest input is not newer than the stamp. Compare just the newest input
(`ls -t | head -1`), which costs one stat sweep. The existing examples are
`$(PKGROOT)/.installed`, `.pkg-libs.stamp`, `.soname-copies.stamp` and
`.soname-prune.stamp`:

```make
	@stamp="$(BUILD_DIR)/.my-step.stamp"; \
	newest=$$(ls -t <the inputs> 2>/dev/null | head -1); \
	if [ -n "$$newest" ] && [ -f "$$stamp" ] && [ -d <the destination> ] && \
	   [ ! "$$newest" -nt "$$stamp" ]; then \
		exit 0; \
	fi; \
	... the loop ...; \
	touch "$$stamp"
```

## A smoke lane states its identity

A lane's name defaults to `B1NIX_ISO_NAME`, which works only while every lane
has an image of its own. A lane that boots another lane's image (as `sysnet`
boots `sys`'s) must set `SMOKE_LANE` itself. Otherwise its markers are graded
as the other lane's, and all of its own checks look missing.

## No kernel wait is bounded in wall-clock time

On a busy host a guest's vCPUs get only a fraction of a core, so a timeout
inside the kernel has to subtract stolen time. `serial_silence_watchdog()` in
`kernel/sched/scheduler.c` measures silence as ticks minus `g_stolen_ticks`: a
guest that is really wedged accrues no steal and is still caught. Before calling
a failure host flakiness, check whether the failing check measured wall time.

## A run that cannot hang is worth more than a fast one

Every step that leaves this machine's control gets a deadline, and every
deadline says what it was waiting for when it fired.

- Anything entering the Debian chroot (`tools/deb/debian-chroot.sh`) runs
  under `STEP_TIMEOUT`, 900 s by default. apt waiting on a mirror and dpkg
  waiting on a lock it will never get both used to sit until a person noticed.
- A guest is booted through `tools/run/run-distro.sh`, which kills it when the
  console stops producing anything new. The comparison ignores the timestamp on
  each line: the failure that cost most of a day was a process repeating
  `access("/run/systemd/journal/flushed")` thousands of times a second, which
  a plain "has the log grown" test calls healthy.
- `tools/run/debug/limine-set-init.py` rewrites the kernel command line inside a
  built image, so changing a diagnostic flag costs a second rather than a
  rebuild. `tools/image/push-kernel.sh` does the same for the kernel itself.

The rule behind all three: when an experiment can only tell you something by
finishing, make it finish quickly or say why it did not.

## The build tree, and who owns what is in it

Everything this repository produces goes into one directory, `build/`, and
nothing is written beside its sources. That rule is not tidiness: the exception
to it — a sub-make that wrote objects next to its own files — was invisible
while an unanchored `build` pattern in `.gitignore` hid every directory of that
name, and became 238 tracked object files the moment that directory was
renamed.

`tools/toolchain/build-inventory.sh` reports what is in there, split the way
the two halves actually differ:

- **Fetched** — the imported Linux source, the Alpine packages pinned by
  `alpine.lock`, the Debian layer from the registry. Deleting these costs a
  download, which on a slow link is an afternoon.
- **Ours** — objects, the rootfs, the toolchain, the images. Deleting these
  costs a rebuild and nothing else.

It also lists every image and ISO with whether any lane, run script or the
Makefile still names it. An artifact no name reaches is from an investigation
that ended: `--prune` removes those, and the first run of it reclaimed ten
gigabytes of ISOs from work finished months ago. It prints what it deletes,
because "nothing names it" is evidence, not proof — a name can live somewhere
the script does not look.
