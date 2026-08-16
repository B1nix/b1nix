#!/bin/sh
# Fail the build when a rootfs binary cannot resolve what it links against.
#
# Every executable and library in the rootfs is loaded by ld.so at runtime, so
# a missing library or an unresolved symbol is not found at build time — it is
# found by a program dying in the guest with "Error relocating ...: symbol not
# found", usually as a smoke failure in a subsystem that has nothing to do with
# the real cause. That is exactly how a torn rootfs copy (one build writing it
# while another reads it) presented itself: fourteen network checks failing on
# a libcurl whose libcrypto was half-written.
#
# This walks the rootfs the way the loader would:
#   * every DT_NEEDED must exist in the rootfs library path
#   * every undefined, non-weak dynamic symbol must be defined somewhere in
#     that binary's own DT_NEEDED closure
#
# Symbol versions are ignored, because musl's loader ignores them too: it
# matches on the bare name, so verifying anything stricter here would reject
# images that actually run.
#
# Usage: sh tools/check-rootfs-links.sh [rootfs-dir]

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
[ -f "$PROJECT_DIR/Makefile" ] || PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCH="${ARCH:-x86_64}"
ROOTFS="${1:-$PROJECT_DIR/build/$ARCH/rootfs}"

# A gate that passes when it cannot run is worse than no gate: it reports
# success for an image nobody checked. Both of these are build failures.
[ -d "$ROOTFS" ] || { echo "check-rootfs-links: no rootfs at $ROOTFS" >&2; exit 1; }
PY=$(command -v python3 || true)
[ -n "$PY" ] || { echo "check-rootfs-links: python3 not found" >&2; exit 1; }

"$PY" - "$ROOTFS" <<'PYEOF'
import os, struct, sys

rootfs = sys.argv[1]

DT_NEEDED, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT, DT_HASH, DT_GNU_HASH = 1, 5, 6, 10, 11, 4, 0x6ffffef5
DT_RPATH, DT_RUNPATH = 15, 29
PT_DYNAMIC, PT_LOAD = 2, 1
STB_WEAK, SHN_UNDEF = 2, 0


class Elf:
    """Just enough ELF64 to answer: what do you need, what do you define, what
    do you leave undefined."""

    @staticmethod
    def _symcount(data, v2o, hash_off, gnu_hash_off, symtab, syment):
        """Number of entries in .dynsym, from the hash table that indexes it."""
        if hash_off is not None and hash_off + 8 <= len(data):
            _nbucket, nchain = struct.unpack_from('<II', data, hash_off)
            return nchain  # DT_HASH chains one slot per symbol, in order
        if gnu_hash_off is not None and gnu_hash_off + 16 <= len(data):
            nbuckets, symoffset, bloom_size, _shift = struct.unpack_from('<IIII', data, gnu_hash_off)
            buckets_at = gnu_hash_off + 16 + bloom_size * 8
            chain_at = buckets_at + nbuckets * 4
            last = symoffset
            for i in range(nbuckets):
                off = buckets_at + i * 4
                if off + 4 > len(data):
                    break
                b, = struct.unpack_from('<I', data, off)
                if b > last:
                    last = b
            # Walk that bucket's chain to its terminator to find the highest index.
            while True:
                off = chain_at + (last - symoffset) * 4
                if off + 4 > len(data):
                    break
                v, = struct.unpack_from('<I', data, off)
                if v & 1:
                    break
                last += 1
            return last + 1
        return 0

    def __init__(self, path):
        self.path = path
        self.needed = []
        self.runpath = []
        self.defined = set()
        self.undefined = set()
        self.ok = False
        with open(path, 'rb') as fh:
            data = fh.read()
        if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] != 2:
            return
        e_phoff, = struct.unpack_from('<Q', data, 32)
        e_phentsize, e_phnum = struct.unpack_from('<HH', data, 54)
        segs = []
        dyn_off = dyn_sz = None
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            if off + 56 > len(data):
                return
            p_type, = struct.unpack_from('<I', data, off)
            p_offset, p_vaddr = struct.unpack_from('<QQ', data, off + 8)
            p_filesz, = struct.unpack_from('<Q', data, off + 32)
            if p_type == PT_LOAD:
                segs.append((p_vaddr, p_offset, p_filesz))
            elif p_type == PT_DYNAMIC:
                dyn_off, dyn_sz = p_offset, p_filesz
        if dyn_off is None:
            return  # static or an object file; check-dynamic.sh owns that case

        def v2o(vaddr):
            for base, off, sz in segs:
                if base <= vaddr < base + sz:
                    return off + (vaddr - base)
            return None

        strtab = symtab = strsz = None
        hash_off = gnu_hash_off = None
        syment = 24
        needed_off = []
        runpath_off = []
        pos = dyn_off
        while pos + 16 <= min(dyn_off + dyn_sz, len(data)):
            tag, val = struct.unpack_from('<qQ', data, pos)
            pos += 16
            if tag == 0:
                break
            if tag == DT_NEEDED:
                needed_off.append(val)
            elif tag in (DT_RPATH, DT_RUNPATH):
                runpath_off.append(val)
            elif tag == DT_STRTAB:
                strtab = v2o(val)
            elif tag == DT_SYMTAB:
                symtab = v2o(val)
            elif tag == DT_STRSZ:
                strsz = val
            elif tag == DT_SYMENT:
                syment = val
            elif tag == DT_HASH:
                hash_off = v2o(val)
            elif tag == DT_GNU_HASH:
                gnu_hash_off = v2o(val)
        if strtab is None or strsz is None:
            return

        def s(idx):
            end = data.find(b'\0', strtab + idx)
            return data[strtab + idx:end].decode('utf-8', 'replace')

        self.needed = [s(o) for o in needed_off]
        # DT_RUNPATH/DT_RPATH are part of how the loader finds a DT_NEEDED, so
        # they are part of how this gate must look for one. Alpine puts
        # pulseaudio's and pipewire's private libraries in their own
        # directories and points at them from the libraries that need them;
        # ignoring that reported an image that runs as an image that cannot.
        for o in runpath_off:
            for entry in s(o).split(':'):
                if entry:
                    self.runpath.append(entry)

        if symtab is not None:
            # The symbol count comes from the hash table, never from guessing
            # where .dynsym ends: assuming .dynstr follows it walks off by a
            # few bytes on some binaries and every name after that point is
            # read at the wrong offset (which reported "cgetpgrp" for
            # tcgetpgrp — plausible-looking nonsense).
            n = self._symcount(data, v2o, hash_off, gnu_hash_off, symtab, syment)
            for i in range(1, n):
                off = symtab + i * syment
                if off + 24 > len(data):
                    break
                st_name, st_info, _st_other, st_shndx = struct.unpack_from('<IBBH', data, off)
                if st_name == 0 or st_name >= strsz:
                    continue
                name = s(st_name).split('@')[0]
                if not name:
                    continue
                if st_shndx == SHN_UNDEF:
                    if (st_info >> 4) != STB_WEAK:
                        self.undefined.add(name)
                else:
                    self.defined.add(name)
        self.ok = True


libdirs = [os.path.join(rootfs, d) for d in ('lib', 'usr/lib', 'usr/local/lib', 'usr/lib/chromium')]
libindex = {}
for d in libdirs:
    if not os.path.isdir(d):
        continue
    for name in os.listdir(d):
        p = os.path.join(d, name)
        if os.path.isfile(p) and name not in libindex:
            libindex[name] = p

cache = {}
dircache = {}


def runpath_lookup(elf, want):
    """Find `want` in `elf`'s DT_RUNPATH/DT_RPATH, mapped into the rootfs.

    $ORIGIN is the directory of the object holding the entry, exactly as the
    loader expands it; an absolute entry is a guest path, so it is taken
    relative to the rootfs rather than the host filesystem."""
    for entry in elf.runpath:
        entry = entry.replace('$ORIGIN', os.path.dirname(elf.path)).replace('${ORIGIN}',
                                                                            os.path.dirname(elf.path))
        d = entry if entry.startswith(rootfs) else os.path.join(rootfs, entry.lstrip('/'))
        if d not in dircache:
            try:
                dircache[d] = set(os.listdir(d))
            except OSError:
                dircache[d] = set()
        if want in dircache[d]:
            return os.path.join(d, want)
    return None


def load(path):
    real = os.path.realpath(path)
    if real not in cache:
        cache[real] = Elf(real)
    return cache[real]


targets = []
for base, _dirs, files in os.walk(rootfs):
    for name in files:
        p = os.path.join(base, name)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        try:
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF':
                    continue
        except OSError:
            continue
        targets.append(p)

# A library nothing in the image names in DT_NEEDED is a dlopen plugin (zsh's
# modules, PAM modules, Mesa drivers). Its undefined symbols are supplied by
# whichever program loads it, which we cannot know here, so only its own
# library dependencies are checked — claiming its symbols are missing would be
# wrong, and a gate that cries wolf gets switched off.
linked_by_name = set()
for p in targets:
    e = load(p)
    if e.ok:
        linked_by_name.update(e.needed)

missing_libs = []
missing_syms = []
for p in sorted(targets):
    e = load(p)
    if not e.ok:
        continue
    closure = {}
    # Each entry is (name, the object that named it) — a DT_RUNPATH belongs to
    # the object carrying it, so who asked decides where to look.
    queue = [(n, e) for n in e.needed]
    seen = set()
    unresolved = []
    while queue:
        want, asker = queue.pop(0)
        if want in seen:
            continue
        seen.add(want)
        lp = runpath_lookup(asker, want) or libindex.get(want)
        if lp is None:
            unresolved.append(want)
            continue
        dep = load(lp)
        if not dep.ok:
            continue
        closure[want] = dep
        queue.extend((n, dep) for n in dep.needed)
    rel = os.path.relpath(p, rootfs)
    for want in unresolved:
        missing_libs.append((rel, want))
    if unresolved:
        continue  # symbol check would only repeat the same news
    is_plugin = (os.path.basename(p) not in linked_by_name and not e.defined & {'main'}
                 and '.so' in os.path.basename(p))
    if is_plugin:
        continue
    provided = set()
    for dep in closure.values():
        provided |= dep.defined
    gone = sorted(sym for sym in e.undefined if sym not in provided and sym not in e.defined)
    if gone:
        missing_syms.append((rel, gone))

if missing_libs or missing_syms:
    print("check-rootfs-links: FAIL — the image contains binaries that cannot load", file=sys.stderr)
    for rel, want in missing_libs:
        print("  %s: needs %s, which is not in the rootfs" % (rel, want), file=sys.stderr)
    for rel, syms in missing_syms:
        shown = ', '.join(syms[:6]) + (', ...' if len(syms) > 6 else '')
        print("  %s: %d unresolved symbol(s): %s" % (rel, len(syms), shown), file=sys.stderr)
    sys.exit(1)

print("check-rootfs-links: %d ELF files, every library and symbol resolves" % len(targets))
PYEOF
