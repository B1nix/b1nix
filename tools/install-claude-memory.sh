#!/bin/sh
# Install the repo-bundled Claude Code agent memory into THIS host's per-project
# memory directory, so a fresh checkout on a new machine starts with the full
# project history/context that the assistant accumulated elsewhere.
#
# Claude Code keys memory by the project's absolute path: the directory under
# ~/.claude/projects/ is that path with every "/" turned into "-". This script
# derives that name from where the repo actually lives on this host, so it works
# regardless of the clone location.
#
# Memory files live in docs/agent-memory/ (gitignored — they are internal working
# notes, not committed). On a host that does not have them yet, carry them over
# with the b1nix-claude-memory.tar.gz bundle.
#
# Usage:
#   sh tools/install-claude-memory.sh            # repo = git root of this script
#   sh tools/install-claude-memory.sh /path/repo # explicit repo path
#
# Re-run any time docs/agent-memory/ changes.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="${1:-$(cd "$SCRIPT_DIR/.." && pwd)}"
REPO="$(cd "$REPO" && pwd)"

SRC="$REPO/docs/agent-memory"
if [ ! -d "$SRC" ] || [ -z "$(ls "$SRC"/*.md 2>/dev/null)" ]; then
	echo "error: no memory files in $SRC" >&2
	echo "  This host has no live memory yet. Carry it over with the bundle:" >&2
	echo "    tar xzf b1nix-claude-memory.tar.gz && cd b1nix-claude-memory" >&2
	echo "    sh install.sh \"$REPO\"" >&2
	exit 1
fi

# Encode the project path the way Claude Code does: "/" -> "-".
ENC=$(printf '%s' "$REPO" | sed 's#/#-#g')
DEST="$HOME/.claude/projects/$ENC/memory"

mkdir -p "$DEST"
cp "$SRC"/*.md "$DEST"/

COUNT=$(ls "$SRC"/*.md 2>/dev/null | wc -l | tr -d ' ')
echo "Installed $COUNT memory files."
echo "  from: $SRC"
echo "  to:   $DEST"
echo ""
echo "Start a Claude Code session from $REPO and the memory index (MEMORY.md)"
echo "will be loaded automatically."
