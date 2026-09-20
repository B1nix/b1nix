# Boot counting and the two-kernel fallback

The one mechanism in the distribution with no upstream to copy. systemd-boot
has boot counting built in; Limine does not, and Limine cannot write to disk at
all, so every part of this runs in the operating system.

It is also the mechanism a user's machine depends on when a kernel update is
bad, which is the normal case for a from-scratch kernel. It must fail towards
booting, never towards not booting.

## Layout

The ESP is FAT, mounted at `/boot`:

```
/boot/
  EFI/BOOT/BOOTX64.EFI        # Limine
  limine.conf                 # generated, never hand-edited
  b1nix-<release>             # kernel, stripped
  initrd-<release>            # initramfs
  b1nix/boot-state            # the state file, written by us
```

`boot-state` is plain text, one `key=value` per line, because it is parsed by a
shell script in an initramfs where nothing else is available:

```
default=0.124.0
entry=0.124.0 tries=3 good=0
entry=0.123.0 tries=0 good=1
```

`tries` counts down. `good=1` means the entry has completed a boot; a good
entry is never counted again.

## Who writes what

**`b1nix-kernel` postinst**, when release N is installed:

1. Copies kernel and initramfs to the ESP.
2. Adds `entry=N tries=3 good=0` and sets `default=N`.
3. Keeps the previous release's entry, and drops anything older than the two
   most recent plus the last `good=1` entry.
4. Regenerates `limine.conf`: the default first, then the other entries newest
   first, then the rescue entry.
5. Each generated entry passes its own identity on the command line:
   `b1nix.entry=<release>`. The booted entry is then known to the system even
   when the user picked it by hand from the menu, which `default=` cannot tell
   us.

**The initramfs hook**, as early as it can run, before the root filesystem is
mounted read-write:

1. Mount the ESP read-write.
2. Read `b1nix.entry=` from `/proc/cmdline`. No value, or no state file: do
   nothing and continue booting.
3. If that entry has `good=1`: do nothing.
4. Otherwise decrement its `tries`. If it reaches 0, set `default=` to the
   newest entry with `good=1` and regenerate `limine.conf`. Log a line that
   says exactly that, so the reason appears in the boot log of the *next* boot
   attempt too.
5. Sync, unmount, continue booting.

Decrementing here, and not in userspace, is the whole point: a kernel that
panics during device probing never reaches a systemd unit, and a counter that
only decrements on a successful boot counts nothing. The initramfs is the
earliest place that can write to a filesystem.

**`b1nix-boot-good.service`** in the installed system:

- `Type=oneshot`, `RemainAfterExit=yes`, ordered after `graphical.target` on a
  desktop and after `multi-user.target` otherwise, plus a 30-second settle so
  that "the desktop appeared and immediately froze" is not counted as good.
- Sets `good=1` for the booted entry, restores its `tries`, sets `default=` to
  it.
- Failing to write is not a boot failure: it logs and exits 0. A full ESP must
  not stop a machine from booting.

## Rules this has to obey

- **Fail towards booting.** Every step above continues on error. A corrupt
  state file is rewritten from what is actually on the ESP; an unwritable ESP
  is logged and ignored.
- **The rescue entry is never counted** and never becomes the default. It is
  the floor.
- **The last `good=1` kernel is never removed**, by the package manager or by
  the pruning in step 3. `b1nix-kernel`'s `prerm` enforces the same thing from
  the other side.
- **A manual menu choice counts.** The user picking the older kernel once
  should not mark the new one good, and should not spend one of its tries
  either — which is why the entry identity travels on the command line.
- **Three tries, not one.** A single failed boot is often a fluke on real
  hardware — a flaky disk, an unlucky race. Three is enough to distinguish a
  reproducible failure without stranding the user for long.

## How it is proved

`DISTRO-SMOKE` installs a kernel that panics on purpose, then boots the image
three times:

- boots 1–3 attempt the broken entry and decrement,
- the third rewrites the default,
- boot 4 comes up on the previous kernel and the lane asserts on both the log
  line naming the fallback and on `uname -r` reporting the older release.

A run where the fallback happened but the log line is missing is a failure:
the mechanism working silently is not good enough, because the user has to
understand what happened to their machine.
