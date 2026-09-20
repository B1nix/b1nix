# b1nix-report

One command a user runs, one file they attach to an issue. It is also the input
to the hardware compatibility list, so the format is fixed before the first
report arrives from a stranger — reports in an older shape are the ones that
get thrown away.

```
$ b1nix-report
Written: /tmp/b1nix-report-2026-09-20-143022.txt  (48 KiB)
Attach this to your issue. Review it first if you like: it is plain text.
```

## Principles

- **Plain text, one file, readable by the person sending it.** Anyone should be
  able to open it and see what they are about to publish. A tarball of binary
  blobs gets sent by fewer people and trusted by none.
- **Machine-parseable anyway**: a stable `key: value` header block at the top,
  free-form sections below, each introduced by `== NAME ==`. The list generator
  reads the header only.
- **Schema version in the header.** When a field changes meaning, the version
  increments and old reports stay interpretable.
- **No secrets, and say so.** The tool prints what it collected and never
  includes: hostnames the user set, network keys, `/etc/shadow`, filesystem
  UUIDs of removable media, or the contents of `$HOME`. MAC addresses are
  reduced to the OUI (the vendor half) — enough to identify a NIC model,
  useless for tracking a machine.

## Header fields

| Key | Example | Why |
|---|---|---|
| `schema` | `1` | so a parser knows what it holds |
| `b1nix_release` | `1` | which release |
| `b1nix_codename` | `hnylytsi` | |
| `kernel` | `6.6.0-b1nix-0.124.0` | `uname -r` |
| `kernel_build_id` | `a3f1…` | matches the report to `b1nix-kernel-dbg` |
| `arch` | `amd64` | |
| `install_kind` | `installed` / `live` / `phone` | a live-session bug is a different bug |
| `firmware` | `uefi` / `bios` / `abl` | |
| `cpu` | `Intel Core i5-8250U` | from CPUID, not from a table |
| `cpus` | `8` | |
| `ram_mib` | `15800` | |
| `gpu` | `8086:5917` | PCI ids, not marketing names |
| `net` | `8086:24fd, 10ec:8168` | |
| `storage` | `nvme, ahci` | which driver bound, not which disk |
| `root_fs` | `btrfs` | |
| `desktop` | `plasma-wayland` / `plasma-x11` / `none` | |
| `boots` | `yes` | the four states of the compatibility list |
| `installs` | `yes` | |
| `desktop_works` | `partial` | |
| `daily_usable` | `no` | |

The last four are the compatibility list's columns, and they are the only
fields the tool asks the user about. Everything else it reads.

## Sections

`== BOOT LOG ==` the kernel log of this boot, `== DRIVERS ==` what bound to
what, `== PCI ==` and `== USB ==` the raw id lists, `== FAILED UNITS ==`
`systemctl --failed`, `== DISKS ==` types and sizes with no serial numbers,
`== PREVIOUS PANIC ==` if one was saved.

## Size

If the boot log pushes the file over a megabyte, the tool truncates the middle
and says it did. A report nobody can attach to an issue has failed at its one
job.

## Feeding the list

The website's hardware table is generated from submitted headers: one row per
`cpu` + `gpu` + `net` combination, the four states, the release that was tested
and a link to the issue. Rows are never invented from what should work — an
empty table is honest and a wrong one costs someone an evening.
