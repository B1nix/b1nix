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
