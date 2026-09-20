#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Move a path and repoint every reference to it, in one step.
#
#   sh tools/toolchain/relayout.sh OLD NEW
#
# A layout change is two operations that must not be separated: `git mv`, and
# the rewrite of every place that names the old path. Doing them by hand is how
# a lane ends up pointing at a file that no longer exists -- and a lane that
# cannot find its input usually reports "missing marker", which reads exactly
# like a kernel that stopped working.
#
# Only tracked text files are rewritten, and the replacement is anchored on the
# whole old path. Run it with a clean index: everything it touches lands in the
# working tree, and a move is much easier to review as one diff of its own.
# Read wholly before anything runs. A shell reads a script incrementally, so a
# move that rewrites this file while it is executing makes the shell resume in
# the middle of text that has shifted underneath it -- which is exactly what
# happened when this script was used to rename its own directory.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

OLD="${1:?usage: relayout.sh OLD NEW}"
NEW="${2:?usage: relayout.sh OLD NEW}"

[ -e "$OLD" ] || { echo "relayout: $OLD does not exist" >&2; exit 1; }

mkdir -p "$(dirname "$NEW")"
git mv "$OLD" "$NEW"

# Every tracked file that could name a path. Binary files are skipped by grep
# -I; the b1cc submodule is not ours to rewrite.
files=$(git grep -lI -F "$OLD" -- . ':!third_party/b1cc' 2>/dev/null || true)
count=0
SELF="tools/toolchain/relayout.sh"
for f in $files; do
	[ -f "$f" ] || continue
	# Leave this file for last, and only after the loop: see the note above.
	[ "$f" != "$SELF" ] || { self_pending=1; continue; }
	OLD="$OLD" NEW="$NEW" python3 - "$f" <<-'PY'
		import os, sys
		p = sys.argv[1]
		s = open(p).read()
		open(p, "w").write(s.replace(os.environ["OLD"], os.environ["NEW"]))
	PY
	count=$((count + 1))
done

if [ "${self_pending:-0}" = "1" ] && [ -f "$SELF" ]; then
	OLD="$OLD" NEW="$NEW" python3 - "$SELF" <<-'PY'
		import os, sys
		p = sys.argv[1]
		s = open(p).read()
		open(p, "w").write(s.replace(os.environ["OLD"], os.environ["NEW"]))
	PY
	count=$((count + 1))
fi

printf 'relayout: %s -> %s, %d file(s) repointed\n' "$OLD" "$NEW" "$count"
