# Build and smoke conventions

Three rules, each of which exists because breaking it cost real time. They are
written down because the same mistake kept reappearing in a different shape:
one place stamped its work, the next redid it; one lane named itself, the next
was guessed at.

## 1. A build step that spawns a tool per file must be skippable

Recipes that loop over hundreds of files and run `readelf`, `nm` or similar on
each one are the dominant cost of a build that has nothing to do. Measured on
aarch64: **496 libraries, one `readelf` each, 17 s of a 28 s no-op build**,
spent every single run to re-derive an answer that changes only when a package
changes.

Guard them with a stamp:

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

Three properties matter and all three are load-bearing:

- the stamp lives under `$(BUILD_DIR)`, so a wiped build tree loses it too and
  the work is redone rather than wrongly skipped;
- the destination is checked, so a step whose output was removed still runs;
- `ls -t | head -1` compares against the newest input only, which is one
  `stat` sweep rather than one per file.

`build/$(ARCH)/pkgroot/.installed` is the older example of the same idea, and
`.pkg-libs.stamp`, `.soname-copies.stamp` and `.soname-prune.stamp` are the
ones added when this was written down. No-op build: **28 s -> 9 s**.

## 2. A smoke lane states its identity; it is never inferred

The lane name used to be derived from `B1NIX_ISO_NAME`, which works only while
every lane has its own image. `sysnet` reuses `sys`'s image and differs solely
in which half of the tests the guest driver runs — it silently became a second
`sys` lane, the network tests ran nowhere, and **91 checks failed as missing
markers**.

A lane that shares another's image sets `SMOKE_LANE` explicitly. A lane that
produces no output looks exactly like a lane whose tests all vanished, so this
is not a class of bug the suite can catch for you.

## 3. Nothing in the kernel bounds a wait in wall-clock time

Under a busy host a guest vCPU gets a fraction of a core, so wall time and
guest progress come apart. `serial_silence_watchdog()` measured silence in wall
time and **panicked a perfectly healthy machine with "deadlock or hang
detected"**: running four lanes instead of three gave 232 s and 90 blocked
checks, none of which was a real failure.

The timer tick knows how far the clock ran ahead of its own interrupts; that
jump is time this vCPU did not get. Subtract it. The check stays honest in both
directions — a genuinely wedged machine still takes its interrupts, accrues no
steal, and is caught on the same budget.

Before blaming flakiness on "the host was busy", check whether the thing that
fired was measuring wall time. This one was.
