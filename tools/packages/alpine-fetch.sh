#!/bin/sh
# Install an Alpine package into a build prefix, instead of building it here.
#
# Most of tools/ports/ rebuilds, from source, on every fresh checkout, software
# that Alpine already builds for exactly this target: musl, x86_64, dynamically
# linked. Fetching their binary costs a download and removes both the build
# script and the obligation to keep it working — see docs/ports-migration-plan.md.
#
# What this is NOT is a package manager. bpkg is that, it runs in the guest, and
# it verifies Alpine's RSA signatures (docs/bpkg-package-manager.md). This is the
# host-side, image-build-time path, and it pins instead of verifying signatures:
# every package's sha256 is recorded in tools/packages/alpine.lock and checked on
# every later fetch. A build therefore either gets the exact bytes an earlier
# build got, or fails — and adding a package is a reviewable diff to that file,
# not something a mirror can do quietly. Transport is HTTPS with the host's CA
# store, so the first fetch is as trustworthy as the mirror's certificate.
#
# Usage:
#   tools/packages/alpine-fetch.sh <prefix> <package>...
#   ALPINE_LOCK_UPDATE=1 tools/packages/alpine-fetch.sh ...   # record new hashes
#
# Environment:
#   ALPINE_LAYOUT    flat | native   (default: flat)
#                    flat lays the package out as <prefix>/{include,lib}, the
#                    shape the from-source ports produced, for things that are
#                    linked against. native keeps the package's own paths, for
#                    installing a program into an image root.
#   ARCH             b1nix arch      (default: x86_64)
#   ALPINE_RELEASE   branch          (default: v3.20)
#   ALPINE_REPO      main | community (default: main)
#   ALPINE_MIRROR    base URL        (default: dl-cdn.alpinelinux.org)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-${B1NIX_ARCH:-x86_64}}"
ALPINE_RELEASE="${ALPINE_RELEASE:-v3.20}"
ALPINE_REPO="${ALPINE_REPO:-main}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
LOCK="$ROOT_DIR/tools/packages/alpine.lock"

# Which repositories to look in, the caller's first.
#
# A package names one repository, but its dependencies need not live there:
# libjxl is in community and the brotli it needs is in main. So every lookup
# walks the list, and each package is downloaded from the repository it was
# actually found in.
REPOS="$ALPINE_REPO"
for r in main community; do
	case " $REPOS " in *" $r "*) ;; *) REPOS="$REPOS $r" ;; esac
done

[ $# -ge 2 ] || { echo "usage: $0 <prefix> <package>..." >&2; exit 2; }
# Absolute, always: the native-layout copy runs from inside the unpacked package
# so it can walk relative paths, and a relative destination would be resolved
# against that directory instead — which quietly builds the destination path
# inside the temporary tree, once per file.
mkdir -p "$1"
PREFIX="$(cd "$1" && pwd)"; shift

CACHE_ROOT="$ROOT_DIR/build/$ARCH/pkgcache"
# The files this run installed, for the dependency closure below.
INSTALLED="$(mktemp)"
# The package names installed by this run, for the dependency passes below.
PKGS_SEEN="$(mktemp)"
trap 'rm -f "$INSTALLED" "$PKGS_SEEN" "$PRESENT"' EXIT

#
# Dependencies that must not be followed into this image.
#
# Alpine packages depend on Alpine's base system, and it is not this one: its
# busybox owns /bin, alpine-baselayout owns /etc, and musl would be a second
# libc beside the one built here. Following those turns installing a compositor
# into replacing the operating system underneath it. openrc and runit are named
# for the reasons in alpine-ports.map. Extendable per invocation.
#
ALPINE_SKIP_DEPS="${ALPINE_SKIP_DEPS:-busybox busybox-binsh alpine-baselayout \
alpine-baselayout-data alpine-keys alpine-release musl musl-utils musl-locales \
libc6-compat scanelf ssl_client openrc runit}"
mkdir -p "$PREFIX"

fetch() {
	curl -fsSL --retry 3 -o "$2" "$1" || {
		echo "alpine-fetch: cannot download $1" >&2
		return 1
	}
}

# The index is one text file inside a gzipped tar. It is refreshed at most once
# per build: it names the current version of every package, and a stale copy
# would ask for a version the mirror has already retired.
index() {
	repo="$1"
	dir="$CACHE_ROOT/$ALPINE_RELEASE-$repo"
	idx="$dir/APKINDEX"
	if [ ! -f "$idx" ]; then
		mkdir -p "$dir"
		fetch "$ALPINE_MIRROR/$ALPINE_RELEASE/$repo/$ARCH/APKINDEX.tar.gz" \
		      "$dir/APKINDEX.tar.gz"
		tar -xzOf "$dir/APKINDEX.tar.gz" APKINDEX > "$idx"
	fi
	echo "$idx"
}

# The version of one package, from the blank-line-separated records. Matching on
# the whole P: line: "zlib" must not match "zlib-dev".
pkg_version_in() {
	awk -v want="$2" -v RS= '
		{
			name = ""; ver = ""
			n = split($0, lines, "\n")
			for (i = 1; i <= n; i++) {
				if (lines[i] ~ /^P:/) name = substr(lines[i], 3)
				if (lines[i] ~ /^V:/) ver  = substr(lines[i], 3)
			}
			if (name == want) { print ver; exit }
		}' "$(index "$1")"
}

# The repository a package is in, searched in REPOS order. Prints "repo version".
pkg_locate() {
	for r in $REPOS; do
		v="$(pkg_version_in "$r" "$1")"
		[ -n "$v" ] && { echo "$r $v"; return; }
	done
}

locked_sha() {
	[ -f "$LOCK" ] || return 0
	awk -v n="$1" -v v="$2" -v a="$ARCH" \
	    '$1 == n && $2 == v && $3 == a { print $4; exit }' "$LOCK"
}

record_sha() {
	mkdir -p "$(dirname "$LOCK")"
	printf '%s %s %s %s\n' "$1" "$2" "$ARCH" "$3" >> "$LOCK"
	sort -o "$LOCK" "$LOCK"
}

#
# Which package provides a given SONAME.
#
# Every record in the index lists what it provides, and a shared library shows
# up there as `so:libfoo.so.1=version`. That is how a DT_NEEDED entry is turned
# back into something installable — the same lookup bpkg does in the guest.
#
pkg_for_soname_in() {
	awk -v want="$2" -v RS= '
		{
			name = ""; found = 0
			n = split($0, lines, "\n")
			for (i = 1; i <= n; i++) {
				if (lines[i] ~ /^P:/) name = substr(lines[i], 3)
				if (lines[i] ~ /^p:/) {
					split(substr(lines[i], 3), provs, " ")
					for (j in provs) {
						split(provs[j], kv, "=")
						if (kv[1] == "so:" want) found = 1
					}
				}
			}
			if (found) { print name; exit }
		}' "$(index "$1")"
}

pkg_for_soname() {
	for r in $REPOS; do
		n="$(pkg_for_soname_in "$r" "$1")"
		[ -n "$n" ] && { echo "$n"; return 0; }
	done
	#
	# "Nothing provides it" is an answer, not a failure.
	#
	# Without this the function ended on a false test, so it returned 1, and
	# the caller's `provider="$(pkg_for_soname ...)"` took the whole script
	# down under `set -eu` before it could reach the branch that reports the
	# name and carries on -- silently, since the exit carried no message. The
	# caller already handles an empty answer, and the comment above the closure
	# says an unresolvable SONAME is deliberately not fatal, because the image
	# supplies several libraries under names Alpine also packages.
	#
	return 0
}

#
# What a package says it depends on, in its own words.
#
# The SONAME closure below finds libraries, and only libraries. A dependency on
# another *program* is invisible to it: cage links nothing against Xwayland, it
# execs it, so cage arrived complete by every measure this script had and then
# refused to start with "Cannot create XWayland server". The index says so
# plainly in the record's D: line, which is read here.
#
pkg_deps_in() {
	awk -v want="$2" -v RS= '
		{
			name = ""; deps = ""
			n = split($0, lines, "\n")
			for (i = 1; i <= n; i++) {
				if (lines[i] ~ /^P:/) name = substr(lines[i], 3)
				if (lines[i] ~ /^D:/) deps = substr(lines[i], 3)
			}
			if (name == want) { print deps; exit }
		}' "$(index "$1")"
}

# Which package provides a command, for the `cmd:` form of a dependency.
pkg_for_cmd_in() {
	awk -v want="$2" -v RS= '
		{
			name = ""; found = 0
			n = split($0, lines, "\n")
			for (i = 1; i <= n; i++) {
				if (lines[i] ~ /^P:/) name = substr(lines[i], 3)
				if (lines[i] ~ /^p:/) {
					split(substr(lines[i], 3), provs, " ")
					for (j in provs) {
						split(provs[j], kv, "=")
						if (kv[1] == "cmd:" want) found = 1
					}
				}
			}
			if (found) { print name; exit }
		}' "$(index "$1")"
}

# Which package provides a plain name, for a dependency that is not a package.
#
# apk lets several packages answer to one name: plasma-workspace depends on
# "pipewire-session-manager", and no package is called that -- wireplumber and
# pipewire-media-session each list it under `p:`. Looking only at `P:` makes
# such a dependency read as missing from the repository, which is what it did.
pkg_for_provide_in() {
	awk -v want="$2" -v RS= '
		{
			name = ""; found = 0
			n = split($0, lines, "\n")
			for (i = 1; i <= n; i++) {
				if (lines[i] ~ /^P:/) name = substr(lines[i], 3)
				if (lines[i] ~ /^p:/) {
					split(substr(lines[i], 3), provs, " ")
					for (j in provs) {
						split(provs[j], kv, "=")
						if (kv[1] == want) found = 1
					}
				}
			}
			if (found) { print name; exit }
		}' "$(index "$1")"
}

pkg_for_provide() {
	for r in $REPOS; do
		got="$(pkg_for_provide_in "$r" "$1")"
		[ -n "$got" ] && { echo "$got"; return; }
	done
}

pkg_for_cmd() {
	for r in $REPOS; do
		n="$(pkg_for_cmd_in "$r" "$1")"
		[ -n "$n" ] && { echo "$n"; return; }
	done
}

install_one() {
	name="$1"
	set -- $(pkg_locate "$name")
	repo="${1:-}"; ver="${2:-}"
	[ -n "$ver" ] || {
		echo "alpine-fetch: $name is in none of ($REPOS) for $ALPINE_RELEASE/$ARCH" >&2
		exit 1
	}

	CACHE="$CACHE_ROOT/$ALPINE_RELEASE-$repo"
	mkdir -p "$CACHE"
	apk="$CACHE/$name-$ver.apk"
	[ -f "$apk" ] || fetch \
		"$ALPINE_MIRROR/$ALPINE_RELEASE/$repo/$ARCH/$name-$ver.apk" "$apk"

	got="$(sha256sum "$apk" | cut -d' ' -f1)"
	want="$(locked_sha "$name" "$ver")"
	if [ -z "$want" ]; then
		if [ "${ALPINE_LOCK_UPDATE:-0}" = "1" ]; then
			record_sha "$name" "$ver" "$got"
			want="$got"
		else
			echo "alpine-fetch: $name-$ver ($ARCH) is not in alpine.lock." >&2
			echo "  Re-run with ALPINE_LOCK_UPDATE=1 to record $got" >&2
			exit 1
		fi
	fi
	if [ "$got" != "$want" ]; then
		echo "alpine-fetch: $name-$ver does not match alpine.lock" >&2
		echo "  expected $want" >&2
		echo "  got      $got" >&2
		rm -f "$apk"
		exit 1
	fi

	#
	# An .apk is three concatenated gzip members — signature, control, data — and
	# tar walks all three as one stream. That extracts the control entries too;
	# they are all dot-prefixed at the archive root, so they are named exactly
	# and removed, rather than being filtered by a pattern that could also match
	# a real file.
	#
	tmp="$CACHE/.x-$name"
	rm -rf "$tmp"; mkdir -p "$tmp"
	tar -xzf "$apk" -C "$tmp" 2>/dev/null || {
		echo "alpine-fetch: cannot unpack $apk" >&2
		exit 1
	}
	rm -f "$tmp/.PKGINFO" "$tmp/.SIGN."* "$tmp/.pre-install" \
	      "$tmp/.post-install" "$tmp/.pre-upgrade" "$tmp/.post-upgrade" \
	      "$tmp/.pre-deinstall" "$tmp/.post-deinstall" "$tmp/.trigger"

	#
	# Alpine's layout, flattened to the one the from-source ports produce:
	# <prefix>/include and <prefix>/lib. Consumers name those two paths and
	# nothing else, so a package can replace a port without touching them.
	#
	if [ "${ALPINE_LAYOUT:-flat}" = native ]; then
		#
		# The package's own paths, straight into the destination.
		#
		# A program is not a build input: it looks for its own files at the
		# paths it was compiled with — zsh's modules under /usr/lib/zsh, a
		# terminfo database, a configuration file — so flattening lib and
		# usr/lib into one directory would leave a binary that starts and then
		# cannot find itself.
		#
		#
		# Directories first, then files, one at a time.
		#
		# A plain recursive copy replaces a directory in the destination, and in
		# an image root some of those are symlinks — /usr/lib points at /lib —
		# so it fails outright rather than merging into them. mkdir -p on a
		# symlink to a directory succeeds and changes nothing, which is exactly
		# the behaviour wanted: the package's files land wherever that link
		# already goes.
		#
		(cd "$tmp" && find . -type d -exec mkdir -p "$PREFIX/{}" \;)
		(cd "$tmp" && find . ! -type d -exec cp -a --remove-destination \
			"{}" "$PREFIX/{}" \;)
		(cd "$tmp" && find . ! -type d) | sed "s|^\.|$PREFIX|" >> "$INSTALLED"
		rm -rf "$tmp"
		echo "$name" >> "$PKGS_SEEN"
		echo "ALPINE $name-$ver [$ARCH] -> $PREFIX (native layout)"
		return
	fi

	mkdir -p "$PREFIX/include" "$PREFIX/lib"
	[ -d "$tmp/usr/include" ] && cp -a --remove-destination "$tmp/usr/include/." "$PREFIX/include/"
	#
	# usr/lib first, lib second, so real files win.
	#
	# A package can have the same name in both: openssl ships the real
	# libcrypto.so.3 in /lib and, in /usr/lib, a symlink to ../../lib/. Copying
	# usr/lib last replaced the file with that symlink, and flattening the two
	# directories into one made the symlink point outside the prefix at a path
	# that does not exist here. This order keeps the file.
	#
	# --remove-destination, because a name already there may be a symlink into a
	# directory this flattening removed: plain cp would try to write through it
	# and refuse.
	[ -d "$tmp/usr/lib" ] && cp -a --remove-destination "$tmp/usr/lib/." "$PREFIX/lib/"
	[ -d "$tmp/lib" ] && cp -a --remove-destination "$tmp/lib/." "$PREFIX/lib/"
	[ -d "$tmp/usr/sbin" ] && { mkdir -p "$PREFIX/sbin"; cp -a --remove-destination "$tmp/usr/sbin/." "$PREFIX/sbin/"; }
	[ -d "$tmp/sbin" ] && { mkdir -p "$PREFIX/sbin"; cp -a --remove-destination "$tmp/sbin/." "$PREFIX/sbin/"; }
	[ -d "$tmp/usr/bin" ] && { mkdir -p "$PREFIX/bin"; cp -a --remove-destination "$tmp/usr/bin/." "$PREFIX/bin/"; }
	[ -d "$tmp/bin" ] && { mkdir -p "$PREFIX/bin"; cp -a --remove-destination "$tmp/bin/." "$PREFIX/bin/"; }
	find "$PREFIX" -type f -o -type l 2>/dev/null >> "$INSTALLED"
	rm -rf "$tmp"

	# Symlinks that named another directory now name a sibling, since there is
	# only one directory left. Retargeted rather than followed: a link is how
	# the SONAME and the -dev name stay distinct entries.
	for l in "$PREFIX"/lib/*.so*; do
		[ -L "$l" ] || continue
		target="$(readlink "$l")"
		case "$target" in
		*/*)
			base="$(basename "$target")"
			[ -e "$PREFIX/lib/$base" ] && ln -sf "$base" "$l"
			;;
		esac
	done

	echo "$name" >> "$PKGS_SEEN"
	echo "ALPINE $name-$ver [$ARCH] -> $PREFIX"
}

for name in "$@"; do
	install_one "$name"
done

#
# What the packages themselves say they need, beyond libraries.
#
# A D: entry can be a library (`so:`), a build-time name (`pc:`), a file, or a
# package — and only the first of those is reachable from an ELF. The package
# and `cmd:` forms are what put a *program* on the image: cage execs Xwayland
# and links nothing against it, so nothing else in this script could have known.
#
#
# Native layout only, and never a -dev package.
#
# This closure answers "what else must be on the image for this program to
# run". A flat prefix is the other question — what a link line needs — and
# there the same walk drags in the -dev package of every dependency and the
# -dev packages of those, which is both useless to a linker that already has
# the libraries and a large addition to alpine.lock.
#
# Each package's dependencies are read ONCE.
#
# The loop used to re-read every seen package on every round: eight rounds over
# three hundred packages, each read an awk scan of a multi-megabyte index, so
# the KDE closure took hours and the run was usually killed before it produced
# an image. Expanding a package a second time cannot add anything the first
# expansion missed, so the rounds now only look at what the previous round
# newly installed, and the work becomes linear in the number of packages.
PKGS_EXPANDED="$(mktemp)"
: >"$PKGS_EXPANDED"
round=0
while [ "${ALPINE_LAYOUT:-flat}" = native ] && [ "$round" -lt 8 ]; do
	round=$((round + 1))
	added=0
	for pkg in $(sort -u "$PKGS_SEEN" | grep -vxF -f "$PKGS_EXPANDED" 2>/dev/null ||
		     sort -u "$PKGS_SEEN"); do
		echo "$pkg" >>"$PKGS_EXPANDED"
		set -- $(pkg_locate "$pkg")
		[ -n "${1:-}" ] || continue
		for dep in $(pkg_deps_in "$1" "$pkg"); do
			# Version constraints and conflicts are not selections.
			case "$dep" in
			!*) continue ;;
			so:*|pc:*|/*) continue ;;
			esac
			dep="${dep%%[<>=~]*}"
			case "$dep" in
			cmd:*) dep="$(pkg_for_cmd "${dep#cmd:}")" ;;
			esac
			[ -n "$dep" ] || continue
			# Headers and .pc files are a build-time concern; nothing on the
			# image runs them.
			case "$dep" in *-dev) continue ;; esac
			case " $ALPINE_SKIP_DEPS " in *" $dep "*) continue ;; esac
			grep -qx "$dep" "$PKGS_SEEN" && continue
			if [ -z "$(pkg_locate "$dep")" ]; then
				alt="$(pkg_for_provide "$dep")"
				if [ -n "$alt" ]; then
					echo "alpine-fetch: $dep is provided by $alt" >&2
					dep="$alt"
					grep -qx "$dep" "$PKGS_SEEN" && continue
				fi
			fi
			[ -n "$(pkg_locate "$dep")" ] || {
				echo "alpine-fetch: $pkg needs $dep, which is in none of ($REPOS)" >&2
				continue
			}
			install_one "$dep"
			added=1
		done
	done
	[ "$added" = 1 ] || break
done

#
# Whatever the installed libraries themselves need.
#
# A package names its shared-library dependencies in DT_NEEDED and Alpine ships
# them separately: freetype wants libbz2, harfbuzz wants glib and graphite2, and
# glib in turn wants several more. Chasing that by hand means discovering each
# missing library from a guest that failed to start, one boot at a time, so it
# is closed here instead — every SONAME that is not already in the prefix is
# looked up in the index and installed, until nothing new appears.
#
# libc is the exception: the guest has musl already, under Alpine's SONAME as
# well (see the root-image rule), and installing theirs would put a second libc
# on the image.
#
#
# What the installed files themselves need.
#
# Only the files this run put there are examined. In the native layout the
# destination is an image root that already holds libraries of its own — libc
# under three names, the graphics stack, everything built here — and walking all
# of it would ask the index for things Alpine never packaged.
#
# A SONAME already present anywhere in the destination is satisfied. One that is
# neither present nor in the index is reported and left alone rather than
# treated as fatal: the image supplies several libraries under names Alpine also
# uses, and the link that needs it will say so plainly if it is genuinely
# absent.
#
round=0
PRESENT="$(mktemp)"
while [ "$round" -lt 16 ]; do
	round=$((round + 1))
	missing=""
	#
	# One index of what the destination already holds, built once per round.
	#
	# The test below used to be a `find "$PREFIX" -name "$need"` per SONAME.
	# That is a full walk of the staging root for every library every binary
	# names, and the KDE group's staging root is 1.2 GB: on a rebuild, where
	# almost every SONAME is already present and so every walk runs to
	# completion, the pass ran for over an hour and pulled nothing. Basenames
	# are all the test ever looked at, so collect them once and ask a sorted
	# file instead.
	find "$PREFIX" \( -type f -o -type l \) 2>/dev/null |
		sed 's|.*/||' | sort -u > "$PRESENT"
	while IFS= read -r so; do
		[ -f "$so" ] || continue
		#
		# Programs name their libraries the same way libraries do.
		#
		# Only files called *.so* used to be examined, so a package whose whole
		# point is an executable brought none of what it links against: sway
		# arrived without wlroots, wayland, pixman or libinput, and the first
		# sign of it was a compositor that could not start. An ELF is an ELF —
		# what matters is DT_NEEDED, not the suffix.
		case "$so" in
		*.so|*.so.*) ;;
		*)
			readelf -hW "$so" 2>/dev/null |
				grep -qE 'Type:[[:space:]]+(EXEC|DYN)' || continue
			;;
		esac
		for need in $(readelf -dW "$so" 2>/dev/null |
		              sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
			case "$need" in libc.musl-*|ld-musl-*) continue ;; esac
			if grep -qxF -- "$need" "$PRESENT"; then
				continue
			fi
			case " $missing " in *" $need "*) continue ;; esac
			missing="$missing $need"
		done
	done < "$INSTALLED"

	[ -n "$missing" ] || break

	added=0
	for need in $missing; do
		provider="$(pkg_for_soname "$need")"
		if [ -z "$provider" ]; then
			echo "alpine-fetch: nothing in the index provides $need" >&2
			continue
		fi
		echo "alpine-fetch: $need -> $provider" >&2
		install_one "$provider"
		added=1
	done
	[ "$added" = 1 ] || break
done

for l in "$PREFIX"/lib/*.so; do
	[ -L "$l" ] || continue
	[ -e "$l" ] && continue
	#
	# Take the archive with it.
	#
	# Leaving the archive behind is what makes this dangerous: the linker would
	# find libfoo.a in the same directory and use it without a word, which is
	# how a missing libexpat package turned into a relocation error in an
	# unrelated shared object. With neither present, naming that library is a
	# link error that says so.
	#
	# Not an error by itself, because a -dev package brings symlinks for every
	# variant its library was split into — harfbuzz has four, and asking for
	# harfbuzz-cairo would drag in cairo, glib and icu for a library nothing
	# here links.
	#
	rm -f "$l" "${l%.so}.a"
	echo "alpine-fetch: $(basename "$l") -> $(readlink "$l" 2>/dev/null); not installed, dropped with its archive" >&2
done

#
# An archive with no code in it.
#
# Alpine builds some packages with GCC's slim LTO, where the .a holds GCC's
# intermediate representation and nothing else — no machine code, no real
# symbols. GCC with its LTO plugin can link that; ld.lld cannot, and it says so
# in the least helpful way available, by reporting every symbol as undefined
# while the archive sits right there on the command line and `nm` cheerfully
# lists the symbols from its index.
#
# The marker is the __gnu_lto_slim symbol, and the only sound thing to do with
# such an archive is to remove it: the shared library in the same package is
# real, and a link that names the library will find it. An archive left in place
# would be chosen over that shared library and fail for reasons that look like
# anything but this.
#
# Fat LTO objects — code and IR in the same member — carry the LTO sections but
# not this symbol, and link fine. They are deliberately left alone.
#
for a in "$PREFIX"/lib/*.a; do
	[ -f "$a" ] || continue
	member="$(ar t "$a" 2>/dev/null | head -1)"
	[ -n "$member" ] || continue
	ar p "$a" "$member" > "$PREFIX/.member.o" 2>/dev/null || continue
	if readelf -sW "$PREFIX/.member.o" 2>/dev/null | grep -q "__gnu_lto_slim"; then
		rm -f "$a"
		echo "alpine-fetch: $(basename "$a") is a slim-LTO archive with no code; dropped, use the shared library" >&2
	fi
	rm -f "$PREFIX/.member.o"
done
