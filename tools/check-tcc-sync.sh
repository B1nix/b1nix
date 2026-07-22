#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUBMODULE="$ROOT_DIR/userspace/tcc"

if [ "${TCC_SYNC_CHECK:-1}" = "0" ]; then
    exit 0
fi

if [ ! -d "$SUBMODULE/.git" ] && [ ! -f "$SUBMODULE/.git" ]; then
    echo "tcc submodule is not initialized. Run: git submodule update --init -- userspace/tcc" >&2
    exit 1
fi

if [ -n "$(git -C "$SUBMODULE" status --porcelain)" ]; then
    echo "tcc submodule has uncommitted changes; refusing to sync automatically." >&2
    echo "Commit/stash them, then rerun the build." >&2
    exit 1
fi

git -C "$SUBMODULE" fetch --quiet origin b1nix-main
local_head="$(git -C "$SUBMODULE" rev-parse HEAD)"
remote_head="$(git -C "$SUBMODULE" rev-parse origin/b1nix-main)"
if [ "$local_head" != "$remote_head" ]; then
    echo "tcc is stale: local $local_head, origin/b1nix-main $remote_head" >&2
    echo "Run: git -C userspace/tcc checkout --detach origin/b1nix-main" >&2
    echo "Then: git add userspace/tcc && git commit -m 'Update tcc submodule'" >&2
    exit 1
fi
