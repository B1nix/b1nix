# Build and smoke conventions

## 1. A per-file tool loop must be skippable

Recipes that run `readelf`/`nm`/etc. once per file over hundreds of files
dominate no-op build time. Guard them with a stamp:

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

- Stamp under `$(BUILD_DIR)`, so wiping the build tree redoes the work.
- Check the destination exists, so removed output is regenerated.
- Compare only the newest input (`ls -t | head -1`): one stat sweep.

Existing examples: `$(PKGROOT)/.installed`, `.pkg-libs.stamp`,
`.soname-copies.stamp`, `.soname-prune.stamp`.

## 2. A smoke lane states its identity

Lane names default to `B1NIX_ISO_NAME`, which only works when each lane has its
own image. A lane that reuses another's image (e.g. `sysnet` on `sys`'s) must
set `SMOKE_LANE` explicitly; otherwise its markers are graded as the other lane
and its own checks all appear "missing".

## 3. No kernel wait is bounded in wall-clock time

On a busy host, guest vCPUs get a fraction of a core. Timeouts in the kernel
must subtract stolen time: `serial_silence_watchdog()`
(`kernel/sched/scheduler.c`) measures silence as ticks minus `g_stolen_ticks`.
A genuinely wedged guest accrues no steal and is still caught. Before calling a
failure "host flakiness", check whether the check measured wall time.
