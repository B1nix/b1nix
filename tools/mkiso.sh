#!/bin/sh
# tools/mkiso.sh - build a bootable b1nix ISO with the Limine bootloader.
#
# Replaces grub-mkrescue (GPLv3) with Limine (BSD-2-Clause) + xorriso: it stages
# the kernel, the Multiboot2 modules and Limine's own boot files into an ISO
# root, expands boot/limine/limine.conf.in, then produces a BIOS+UEFI hybrid ISO
# and installs the BIOS boot stages into it.
#
#   tools/mkiso.sh --stage DIR --out ISO --arch ARCH --kernel PATH \
#                  [--cmdline STR] [--timeout N] [--module PATH:NAME]...
#
#   --stage    ISO root directory (created/reused; kernel + modules land here)
#   --out      output .iso path
#   --arch     x86 | x86_64 (menu titles only)
#   --kernel   kernel ELF to boot (copied to <stage>/boot/kernel.elf)
#   --cmdline  kernel command line (default: empty)
#   --timeout  boot-menu timeout in seconds, 0 = boot entry 1 instantly
#   --module   Multiboot2 module as "<host path>:<module string>"; repeatable.
#              The module string is what the kernel matches on (e.g. rootfs.img),
#              and doubles as the file name inside <stage>/boot/.
#
# Limine's boot files are taken from `limine --print-datadir` when the host CLI
# supports it, else from the usual distro locations; override with LIMINE_DATADIR.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# The @MODULES@ template expansion below passes a multi-line value via
# `awk -v var=...`. BSD/"one true" awk (macOS's /usr/bin/awk) rejects a
# literal newline inside a -v assignment ("newline in string"); GNU awk
# accepts it. Prefer gawk when present rather than assuming the platform's
# awk is GNU awk.
AWK_BIN="$(command -v gawk 2>/dev/null || command -v awk)"

STAGE=""; OUT=""; ARCH="x86_64"; KERNEL=""; CMDLINE=""; TIMEOUT="0"
MODULES=""   # newline-separated "path:name" entries

while [ $# -gt 0 ]; do
  case "$1" in
    --stage)   STAGE="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    --arch)    ARCH="$2"; shift 2 ;;
    --kernel)  KERNEL="$2"; shift 2 ;;
    --cmdline) CMDLINE="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --module)  MODULES="$MODULES$2
"; shift 2 ;;
    *) echo "mkiso: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

[ -n "$STAGE" ]  || { echo "mkiso: --stage is required" >&2; exit 2; }
[ -n "$OUT" ]    || { echo "mkiso: --out is required" >&2; exit 2; }
[ -n "$KERNEL" ] || { echo "mkiso: --kernel is required" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "mkiso: kernel not found: $KERNEL" >&2; exit 1; }

command -v xorriso >/dev/null 2>&1 || { echo "mkiso: missing host tool: xorriso"; exit 1; }
LIMINE_BIN="${LIMINE_BIN:-$(command -v limine 2>/dev/null || true)}"
[ -n "$LIMINE_BIN" ] || { echo "mkiso: missing host tool: limine (install the limine bootloader package)"; exit 1; }

# Locate limine-bios.sys / limine-bios-cd.bin / limine-uefi-cd.bin / BOOT*.EFI.
if [ -z "${LIMINE_DATADIR:-}" ]; then
  LIMINE_DATADIR="$("$LIMINE_BIN" --print-datadir 2>/dev/null || true)"
fi
if [ -z "${LIMINE_DATADIR:-}" ] || [ ! -f "$LIMINE_DATADIR/limine-bios-cd.bin" ]; then
  for d in /usr/share/limine /usr/local/share/limine /opt/homebrew/share/limine; do
    if [ -f "$d/limine-bios-cd.bin" ]; then LIMINE_DATADIR="$d"; break; fi
  done
fi
[ -n "${LIMINE_DATADIR:-}" ] && [ -f "$LIMINE_DATADIR/limine-bios-cd.bin" ] || {
  echo "mkiso: cannot find Limine boot files (set LIMINE_DATADIR)" >&2; exit 1; }

# ── stage the ISO root ─────────────────────────────────────────────────────
mkdir -p "$STAGE/boot/limine" "$STAGE/EFI/BOOT"
cp -f "$KERNEL" "$STAGE/boot/kernel.elf"

# Limine's own boot files. limine-bios.sys must live next to limine.conf in one
# of the directories Limine scans (we use /boot/limine); the two *-cd.bin El
# Torito images are referenced by path from the xorriso command line.
cp -f "$LIMINE_DATADIR/limine-bios.sys" \
      "$LIMINE_DATADIR/limine-bios-cd.bin" \
      "$LIMINE_DATADIR/limine-uefi-cd.bin" "$STAGE/boot/limine/"
for efi in BOOTX64.EFI BOOTIA32.EFI BOOTAA64.EFI BOOTRISCV64.EFI; do
  [ -f "$LIMINE_DATADIR/$efi" ] && cp -f "$LIMINE_DATADIR/$efi" "$STAGE/EFI/BOOT/$efi"
done

# ── modules ────────────────────────────────────────────────────────────────
# Copy each module into <stage>/boot/<name> and build the config block that
# every menu entry repeats. --reflink=auto makes the multi-hundred-MB rootfs
# copy free on a CoW filesystem and is silently ignored elsewhere.
MODULE_BLOCK=""
printf '%s' "$MODULES" | while IFS= read -r m; do
  [ -n "$m" ] || continue
  src="${m%:*}"; name="${m##*:}"
  [ -f "$src" ] || { echo "mkiso: module not found: $src" >&2; exit 1; }
  cp --reflink=auto -f "$src" "$STAGE/boot/$name" 2>/dev/null || cp -f "$src" "$STAGE/boot/$name"
done
# The loop above runs in a subshell (pipeline), so rebuild the config text here.
OLDIFS="$IFS"; IFS='
'
for m in $MODULES; do
  [ -n "$m" ] || continue
  name="${m##*:}"
  MODULE_BLOCK="$MODULE_BLOCK    module_path: boot():/boot/$name
    module_string: $name
"
done
IFS="$OLDIFS"
# Drop the trailing newline: the @MODULES@ placeholder occupies a whole line.
MODULE_BLOCK="${MODULE_BLOCK%
}"

# ── expand the config template ─────────────────────────────────────────────
# @MODULES@ is a whole-line placeholder standing for a multi-line block, so it
# is expanded with awk rather than sed (portable across GNU/BSD sed).
"$AWK_BIN" -v timeout="$TIMEOUT" -v arch="$ARCH" -v cmdline="$CMDLINE" -v modules="$MODULE_BLOCK" '
  {
    line = $0
    if (line == "@MODULES@") { if (modules != "") print modules; next }
    gsub(/@TIMEOUT@/, timeout, line)
    gsub(/@ARCH@/, arch, line)
    gsub(/@CMDLINE@/, cmdline, line)
    print line
  }
' "$ROOT_DIR/boot/limine/limine.conf.in" > "$STAGE/boot/limine/limine.conf"

# ── build the hybrid BIOS+UEFI ISO ─────────────────────────────────────────
xorriso -as mkisofs -R -r -J \
  -b boot/limine/limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  -hfsplus -apm-block-size 2048 \
  --efi-boot boot/limine/limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  "$STAGE" -o "$OUT" >/dev/null 2>&1

# Write Limine's BIOS boot stages into the ISO (no-op for UEFI boots).
"$LIMINE_BIN" bios-install "$OUT" --quiet >/dev/null

printf 'created %s (%s)\n' "$OUT" "$(du -sh "$OUT" | cut -f1)"
