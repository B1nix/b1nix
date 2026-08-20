#!/bin/sh
# make-script-fixture.sh -- regenerate the offline .apk fixtures that
# userspace/rootfs-overlay/apkrepo-scripts/ ships, which is what
# /etc/bpkg-smoke.sh drives bpkg's install-script, trigger and upgrade paths
# against.
#
# The fixtures are checked in (the smoke test runs inside a booted b1nix with
# no network), but they are generated rather than hand-carved so the contents
# stay readable and reproducible: every tar is written sorted, with zeroed
# timestamps and numeric owner 0, so re-running this script on the same source
# produces the same bytes.
#
# Two repositories are written, because bpkg resolves one version per name out
# of one APKINDEX -- an upgrade is what happens when the index is refreshed and
# now names a newer release:
#
#   x86_64/     the repository as it stands today (the 1.0 packages)
#   v2/x86_64/  the same repository one release later (the 2.0 packages)
#
# Four packages:
#
#   scripted     1.0 ships .pre-install, .post-install, .post-deinstall and a
#                .trigger armed on /usr/share/scripted-watch/*, plus one data
#                file OUTSIDE that directory -- so installing it alone must
#                not fire its own trigger. 2.0 additionally ships
#                .pre-upgrade/.post-upgrade, and its install scripts are
#                poisoned: they write to a file nothing may contain, so an
#                upgrade that ran them is caught rather than assumed.
#   triggerbait  ships a single file INSIDE the watched directory, so
#                installing it must fire scripted's trigger. That is the case
#                that matters: a trigger belongs to the transaction, not to
#                the package being installed.
#   noupgrade    ships only install scripts in both versions -- the fallback
#                every real Alpine package relies on, where the install
#                scripts run for the upgrade too and read the old version
#                out of $2.
#   failupgrade  2.0's .pre-upgrade exits non-zero, so the upgrade must be
#                abandoned with 1.0 still installed and 2.0's payload never
#                written.
#
# The .apk container is Alpine's: gzip members concatenated, control member
# first, data member last. These are unsigned, and bpkg only accepts an
# unsigned package over file://, which is exactly how the fixture is served.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REPO="$ROOT/userspace/rootfs-overlay/apkrepo-scripts"
OUT1="$REPO/x86_64"
OUT2="$REPO/v2/x86_64"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

rm -rf "$REPO"
mkdir -p "$OUT1" "$OUT2"

# Deterministic tars: sorted names, no timestamps, no owner names. tar -z pipes
# through gzip reading a stream, which writes a zero mtime into the gzip header,
# so the whole member is a function of its contents alone.
TARFLAGS="--sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=ustar"

# pkgdir <name> <version> -- start a fresh package build directory and echo it.
pkgdir() {
	d="$WORK/$1-$2"
	rm -rf "$d"
	mkdir -p "$d/ctl" "$d/data"
	echo "$d"
}

# mkapk <builddir> <outfile> -- pack ctl/ and data/ into a two-member .apk.
mkapk() {
	d=$1
	out=$2
	# shellcheck disable=SC2086
	( cd "$d/ctl" && tar $TARFLAGS -czf "$d/ctl.tar.gz" $(ls -A | sort) )
	# shellcheck disable=SC2086
	( cd "$d/data" && tar $TARFLAGS -czf "$d/data.tar.gz" $(ls -A | sort) )
	cat "$d/ctl.tar.gz" "$d/data.tar.gz" > "$out"
}

# ------------------------------------------------------------- scripted 1.0
d=$(pkgdir scripted 1.0)
mkdir -p "$d/data/usr/share/scripted"

cat > "$d/ctl/.PKGINFO" <<'EOF'
pkgname = scripted
pkgver = 1.0
arch = x86_64
triggers = /usr/share/scripted-watch/*
EOF

# Every script runs with the install root as its working directory, so a
# relative path lands inside it and nowhere else.
cat > "$d/ctl/.pre-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
# A .pre-install runs BEFORE the package's files are unpacked. Record what the
# filesystem actually looked like at this moment rather than asserting it.
if [ -f usr/share/scripted/data.txt ]; then
	echo LATE > var/log/scripted.order
else
	echo EARLY > var/log/scripted.order
fi
echo "pre $1" > var/log/scripted.pre
EOF

cat > "$d/ctl/.post-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
# By now the payload must be on disk; say so only if it really is.
if [ -f usr/share/scripted/data.txt ]; then
	echo "post $1" > var/log/scripted.post
fi
EOF

cat > "$d/ctl/.post-deinstall" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo gone > var/log/scripted.deinstall
EOF

cat > "$d/ctl/.trigger" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "$@" > var/log/scripted.trigger
EOF

echo "scripted payload" > "$d/data/usr/share/scripted/data.txt"
mkapk "$d" "$OUT1/scripted-1.0.apk"

# ------------------------------------------------------------- scripted 2.0
d=$(pkgdir scripted 2.0)
mkdir -p "$d/data/usr/share/scripted"

cat > "$d/ctl/.PKGINFO" <<'EOF'
pkgname = scripted
pkgver = 2.0
arch = x86_64
triggers = /usr/share/scripted-watch/*
EOF

# These two must NOT run on an upgrade -- the upgrade scripts below replace
# them. Both append to one file, so a single existence check catches either.
cat > "$d/ctl/.pre-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "pre-install ran $1 $2" >> var/log/scripted.wrong
EOF

cat > "$d/ctl/.post-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "post-install ran $1 $2" >> var/log/scripted.wrong
EOF

# 2.0's payload is a NEW file, so "has the payload landed?" is a question about
# this upgrade and not about what 1.0 left behind.
cat > "$d/ctl/.pre-upgrade" <<'EOF'
#!/bin/sh
mkdir -p var/log
if [ -f usr/share/scripted/data2.txt ]; then
	echo LATE > var/log/scripted.upgrade-order
else
	echo EARLY > var/log/scripted.upgrade-order
fi
# $1 is the version being installed, $2 the one being replaced.
echo "pre-upgrade $1 $2" > var/log/scripted.preupgrade
EOF

cat > "$d/ctl/.post-upgrade" <<'EOF'
#!/bin/sh
mkdir -p var/log
# Writes nothing unless the new payload is really on disk.
if [ -f usr/share/scripted/data2.txt ]; then
	echo "post-upgrade $1 $2" > var/log/scripted.postupgrade
fi
EOF

cat > "$d/ctl/.post-deinstall" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo gone > var/log/scripted.deinstall
EOF

cat > "$d/ctl/.trigger" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "$@" > var/log/scripted.trigger
EOF

echo "scripted payload v2" > "$d/data/usr/share/scripted/data2.txt"
mkapk "$d" "$OUT2/scripted-2.0.apk"

# ---------------------------------------------------------- triggerbait 1.0
d=$(pkgdir triggerbait 1.0)
mkdir -p "$d/data/usr/share/scripted-watch"

cat > "$d/ctl/.PKGINFO" <<'EOF'
pkgname = triggerbait
pkgver = 1.0
arch = x86_64
EOF

echo bait > "$d/data/usr/share/scripted-watch/bait.txt"
mkapk "$d" "$OUT1/triggerbait-1.0.apk"

# ---------------------------------------------------- noupgrade 1.0 and 2.0
# Ships no upgrade scripts in either version: upgrading it must fall back to
# the install scripts, which is how nearly every Alpine package behaves.
for v in 1.0 2.0; do
	d=$(pkgdir noupgrade "$v")
	mkdir -p "$d/data/usr/share/noupgrade"

	cat > "$d/ctl/.PKGINFO" <<EOF
pkgname = noupgrade
pkgver = $v
arch = x86_64
EOF

	cat > "$d/ctl/.pre-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
if [ -f usr/share/noupgrade/v2.txt ]; then
	echo LATE > var/log/noupgrade.order
else
	echo EARLY > var/log/noupgrade.order
fi
echo "pre $1 $2" > var/log/noupgrade.pre
EOF

	cat > "$d/ctl/.post-install" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "post $1 $2" > var/log/noupgrade.post
EOF

	echo "noupgrade payload $v" > "$d/data/usr/share/noupgrade/v${v%%.*}.txt"
	if [ "$v" = 1.0 ]; then
		mkapk "$d" "$OUT1/noupgrade-1.0.apk"
	else
		mkapk "$d" "$OUT2/noupgrade-2.0.apk"
	fi
done

# -------------------------------------------------- failupgrade 1.0 and 2.0
d=$(pkgdir failupgrade 1.0)
mkdir -p "$d/data/usr/share/failupgrade"
cat > "$d/ctl/.PKGINFO" <<'EOF'
pkgname = failupgrade
pkgver = 1.0
arch = x86_64
EOF
echo "failupgrade payload 1.0" > "$d/data/usr/share/failupgrade/v1.txt"
mkapk "$d" "$OUT1/failupgrade-1.0.apk"

d=$(pkgdir failupgrade 2.0)
mkdir -p "$d/data/usr/share/failupgrade"
cat > "$d/ctl/.PKGINFO" <<'EOF'
pkgname = failupgrade
pkgver = 2.0
arch = x86_64
EOF
cat > "$d/ctl/.pre-upgrade" <<'EOF'
#!/bin/sh
mkdir -p var/log
echo "refused $1 $2" > var/log/failupgrade.refused
exit 1
EOF
cat > "$d/ctl/.post-upgrade" <<'EOF'
#!/bin/sh
mkdir -p var/log
# Only reachable if the aborted .pre-upgrade was ignored.
echo "post-upgrade ran $1 $2" > var/log/failupgrade.wrong
EOF
echo "failupgrade payload 2.0" > "$d/data/usr/share/failupgrade/v2.txt"
mkapk "$d" "$OUT2/failupgrade-2.0.apk"

# ---------------------------------------------------------------- APKINDEXes
mkdir -p "$WORK/idx1" "$WORK/idx2"
cat > "$WORK/idx1/APKINDEX" <<'EOF'
P:scripted
V:1.0
A:x86_64
T:bpkg install-script and trigger fixture
L:MIT

P:triggerbait
V:1.0
A:x86_64
T:drops a file into the directory scripted's trigger watches
L:MIT

P:noupgrade
V:1.0
A:x86_64
T:ships install scripts only, to prove the upgrade falls back to them
L:MIT

P:failupgrade
V:1.0
A:x86_64
T:the version an aborted upgrade has to leave in place
L:MIT
EOF
# shellcheck disable=SC2086
( cd "$WORK/idx1" && tar $TARFLAGS -czf "$OUT1/APKINDEX.tar.gz" APKINDEX )

cat > "$WORK/idx2/APKINDEX" <<'EOF'
P:scripted
V:2.0
A:x86_64
T:ships .pre-upgrade/.post-upgrade, which replace the install scripts
L:MIT

P:noupgrade
V:2.0
A:x86_64
T:ships install scripts only, to prove the upgrade falls back to them
L:MIT

P:failupgrade
V:2.0
A:x86_64
T:its .pre-upgrade fails, so the upgrade must be abandoned
L:MIT
EOF
# shellcheck disable=SC2086
( cd "$WORK/idx2" && tar $TARFLAGS -czf "$OUT2/APKINDEX.tar.gz" APKINDEX )

echo "wrote $OUT1:"
ls -l "$OUT1"
echo "wrote $OUT2:"
ls -l "$OUT2"
