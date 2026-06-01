#!/bin/sh
# Fetch Mozilla CA bundle (curl-maintained) for initramfs.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT_DIR/build/cacert.pem}"
URL="${CACERT_URL:-https://curl.se/ca/cacert.pem}"

mkdir -p "$(dirname "$OUT")"

if command -v curl >/dev/null 2>&1; then
  curl -fsSL "$URL" -o "$OUT"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$OUT" "$URL"
else
  echo "tools/fetch-cacert.sh: need host curl or wget to fetch $URL" >&2
  exit 1
fi
