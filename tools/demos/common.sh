# Shared helpers for tools/demos/*.sh. Sourced, not executed.

# demo_bin_src <root-dir> <basename> [ext...]  — echo the path of a userspace
# program's source, or exit non-zero.
#
# userspace/bin used to be flat, and every demo script here spelled its source
# as userspace/bin/<name>.<ext>. Commit 5f3e5583 grouped the directory by
# purpose (gfx/, smoke/, tools/, helpers/, compiler/) and moved the files, and
# the Makefile prerequisites moved with them — but these scripts did not, so
# each one looked in a directory its source had left. That fails only on a tree
# where the generated .inc does not already exist, i.e. a clean checkout, and it
# fails quietly: the demo drops out of the initramfs and its smoke markers just
# never appear. Search instead of assuming a layout, so the next regrouping
# does not do this again.
demo_bin_src() {
	_root="$1"; _name="$2"; shift 2
	for _ext in "$@"; do
		if [ -f "$_root/userspace/bin/$_name.$_ext" ]; then
			echo "$_root/userspace/bin/$_name.$_ext"; return 0
		fi
	done
	for _ext in "$@"; do
		_hit=$(find "$_root/userspace/bin" -name "$_name.$_ext" -type f 2>/dev/null | head -1)
		if [ -n "$_hit" ]; then echo "$_hit"; return 0; fi
	done
	echo "demo_bin_src: no source for $_name (tried .$* under $_root/userspace/bin)" >&2
	return 1
}
