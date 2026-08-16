#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
[ -f "$ROOT_DIR/Makefile" ] || ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SUBMODULE="$ROOT_DIR/userspace/b1cc"

if [ "${B1CC_SYNC_CHECK:-1}" = "0" ]; then
    exit 0
fi

if [ ! -d "$SUBMODULE/.git" ] && [ ! -f "$SUBMODULE/.git" ]; then
    echo "b1cc submodule is not initialized. Run: git submodule update --init -- userspace/b1cc" >&2
    exit 1
fi

if [ -n "$(git -C "$SUBMODULE" status --porcelain)" ]; then
    echo "b1cc submodule has uncommitted changes; refusing to sync automatically." >&2
    echo "Commit/stash them, then rerun the build." >&2
    exit 1
fi

git -C "$SUBMODULE" fetch --quiet origin main
local_head="$(git -C "$SUBMODULE" rev-parse HEAD)"
remote_head="$(git -C "$SUBMODULE" rev-parse origin/main)"
if [ "$local_head" != "$remote_head" ]; then
    echo "b1cc is stale: local $local_head, origin/main $remote_head" >&2
    echo "Run: git -C userspace/b1cc checkout --detach origin/main" >&2
    echo "Then: git add userspace/b1cc && git commit -m 'Update b1cc submodule'" >&2
    exit 1
fi
