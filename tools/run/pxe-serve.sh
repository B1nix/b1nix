#!/bin/sh
# Boot a real machine on the LAN from this host over PXE, instead of writing a
# USB stick for every build.
#
# dnsmasq answers as a *proxy* DHCP server: the router keeps handing out
# addresses and dnsmasq only adds the boot file, so nothing on the network
# changes. The boot tree is an ISO stage directory (tools/image/mkiso.sh
# --stage): Limine's UEFI and BIOS PXE loaders fetch limine.conf, the kernel and
# the root module from it over TFTP, and the kernel then runs from RAM exactly
# as it does from the stick.
#
# Usage (root, or dnsmasq with cap_net_bind_service,cap_net_raw,cap_net_admin):
#   sudo sh tools/run/pxe-serve.sh <iso-stage-dir> [interface]
#
# The firewall has to let UDP 67, 69 and 4011 in from the LAN, and the target's
# firmware needs network boot enabled (Secure Boot off).
set -eu

STAGE="$(cd "${1:?usage: pxe-serve.sh <iso-stage-dir> [interface]}" && pwd)"
IFACE="${2:-$(ip -4 route show default | awk '{print $5; exit}')}"
LIMINE_DATADIR="${LIMINE_DATADIR:-$(limine --print-datadir 2>/dev/null || echo /usr/share/limine)}"

command -v dnsmasq >/dev/null 2>&1 || { echo "pxe-serve: dnsmasq is not installed" >&2; exit 1; }
[ -f "$STAGE/tools/image/limine/limine.conf" ] || { echo "pxe-serve: $STAGE is not an ISO stage (no tools/image/limine/limine.conf)" >&2; exit 1; }
[ -f "$LIMINE_DATADIR/limine-bios-pxe.bin" ] || { echo "pxe-serve: no limine-bios-pxe.bin in $LIMINE_DATADIR" >&2; exit 1; }

SUBNET=$(ip -4 addr show dev "$IFACE" | awk '/inet /{split($2,a,"/"); print a[1]; exit}')
[ -n "$SUBNET" ] || { echo "pxe-serve: $IFACE has no IPv4 address" >&2; exit 1; }

# The loaders sit next to the tree they boot: UEFI firmware asks for the file
# dnsmasq names, BIOS PXE for the same name with ".0" appended.
cp -f "$LIMINE_DATADIR/limine-bios-pxe.bin" "$STAGE/limine-bios-pxe.0"
cp -f "$LIMINE_DATADIR/BOOTX64.EFI" "$STAGE/BOOTX64.EFI"
chmod -R a+rX "$STAGE"

echo "pxe-serve: serving $STAGE on $IFACE ($SUBNET), Ctrl-C to stop"
exec dnsmasq --no-daemon --port=0 --log-dhcp --pid-file= \
	--dhcp-leasefile="${PXE_LEASEFILE:-${TMPDIR:-/tmp}/b1nix-pxe.leases}" \
	--interface="$IFACE" --bind-interfaces \
	--dhcp-range="$SUBNET,proxy" \
	--pxe-prompt="b1nix",0 \
	--pxe-service=x86PC,"b1nix (BIOS)",limine-bios-pxe \
	--pxe-service=X86-64_EFI,"b1nix (UEFI)",BOOTX64.EFI \
	--enable-tftp --tftp-root="$STAGE"
