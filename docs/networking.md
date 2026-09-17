# Networking

Milestones M6, M10, M23, M32–M32c, M84, M96, M106 and the network half of M109.

## The stack

The stack is the kernel's own: Ethernet and ARP, IPv4 with ICMP, UDP and TCP,
and IPv6 with NDP, SLAAC and MLD (M6, M32a). TCP has the full connection
lifecycle with retransmission, window scaling, SACK and Reno congestion control
(M10, M32). Routing uses an IPv4 and IPv6 FIB with policy rules and
equal-cost multipath (M84). The in-kernel DHCP and DHCPv6 clients configure the
initial namespace, and DNS resolution, NTP and a netlink route interface are
provided for BusyBox's `ip` (M106, M107). IPv6, NDP and NTP are loadable modules
registered with a protocol table, so protocols can come and go at runtime
(M96).

Sockets cover AF_INET and AF_INET6 (stream, datagram and raw ICMP/ICMPv6),
AF_UNIX (stream, datagram and seqpacket, with abstract names and descriptor
passing), AF_NETLINK and AF_PACKET. Link types include veth, VLAN, bridge,
bond and GRE/gretap. TLS clients, `curl`/`wget` and dropbear's SSH server all
run on it, inbound through a QEMU port forward and on bare metal (M32b, M32c).
The drivers are virtio-net over PCI and MMIO, e1000/e1000e and r8169. A laptop
without a serial port sends its kernel log as UDP with
`b1nix.netconsole=<ip>:<port>` (see `drivers-and-graphics.md`).

## Network namespaces

A network interface belongs to exactly one namespace, and a veth pair is the
only way between namespaces. The receive path pushes the arriving interface's
namespace as the current context, so every lookup below it answers for the
right namespace without being told. Sockets are stamped with the namespace they
were created in. UDP ports, TCP connections and listeners, raw ICMP delivery
and AF_PACKET delivery all filter on that stamp. IPv4 and IPv6 routes and the
ARP and NDP caches carry a namespace id too.

Each namespace has one L3 interface: the first device that is administratively
up. The initial namespace keeps the NIC that DHCP chose. An interface has a
primary IPv4 address plus up to seven more, from `ip addr add` or an ifconfig
alias. Each address installs its own on-link route, and deleting the primary
promotes the next one. In IPv6 a namespace has its link-local address (EUI-64 of
the L3 interface), its SLAAC or DHCPv6 address, its gateway and prefix, and any
address added with `ip -6 addr add`.

The in-kernel DHCP clients serve only the initial namespace. In any other
namespace BusyBox's `udhcpc` works: it takes its lease over AF_PACKET, and its
renewing socket gets `SO_BROADCAST` (enforced, so a send without it is EACCES)
and `SO_BINDTODEVICE`.

Configuring a namespace's interfaces, routes and addresses, and opening raw
sockets there, needs `CAP_NET_ADMIN` or `CAP_NET_RAW` over the user namespace
that owns the network namespace. A container's root can therefore bring up its
own loopback. The per-namespace sysctl `net.ipv4.ping_group_range` is stored
and enforced. ICMP datagram ("ping") sockets themselves are not implemented, so
a group inside the range gets EPROTONOSUPPORT rather than a socket that
silently behaves like UDP.
