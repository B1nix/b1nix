#!/bin/sh
# M98 — host-side collector for b1nix netconsole.
#
# Boot the target with, for example:
#
#     b1nix.netconsole=192.168.1.20:6666
#
# where 192.168.1.20 is THIS machine, then run:
#
#     tools/netconsole-collect.sh 6666 | tee boot.log
#
# The kernel ships its log ring as plain UDP datagrams containing raw console
# bytes, so there is no framing to strip: the collector prints payloads in
# arrival order. Datagrams are best-effort — a machine that dies mid-boot may
# lose its last few, which is the tradeoff for not blocking the kernel on the
# network.
set -eu

PORT="${1:-6666}"
BIND="${2:-0.0.0.0}"

if ! command -v python3 >/dev/null 2>&1; then
	echo "netconsole-collect: python3 is required" >&2
	exit 1
fi

echo "netconsole: listening on ${BIND}:${PORT} (Ctrl-C to stop)" >&2

exec python3 - "$BIND" "$PORT" <<'PY'
import socket
import sys

bind, port = sys.argv[1], int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((bind, port))

try:
    while True:
        data, _addr = s.recvfrom(65535)
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
except KeyboardInterrupt:
    pass
PY
