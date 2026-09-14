# Network namespaces

What a network namespace owns in b1nix, and where it lives. Smoke checks:
`tests/smoke.sh`, markers `M109-SMOKE: ok netns-*` (from `userspace/bin/smoke/m109_smoke.c`).

## Isolation model

- An interface belongs to one namespace (`struct netdev::netns`); a veth pair
  is the only way across. Receive paths push the arriving interface's
  namespace as the context (`namespace_net_push_context`), so every lookup
  below answers in the right namespace without being told.
- Each namespace has one L3 interface: the first administratively up device
  it holds (the initial namespace keeps its DHCP-chosen NIC).
- Sockets are stamped with the namespace they were created in; UDP ports, TCP
  connections/listeners, raw ICMP/ICMPv6 and AF_PACKET delivery all filter on it.
- Routes (IPv4 and IPv6), the ARP cache and the NDP cache carry a namespace id.

## IPv4 addresses (`kernel/net/net.c`)

- A primary address (what DHCP binds, `SIOCSIFADDR` on the interface name)
  plus up to seven more: `ip addr add`, or an ifconfig alias (`eth0:1`).
- `ipv4_receive` and ARP answer for every address; the source of a datagram
  (and of an ARP request) is the address on the next hop's prefix.
- Every address installs its own on-link route (`route_configure_interface`);
  deleting the primary promotes the next address.

## IPv6 state

- Per namespace: link-local (EUI-64 of the L3 interface's MAC), the SLAAC /
  DHCPv6 address, gateway/prefix, and addresses assigned with
  `ip -6 addr add` (each with its on-link prefix route).
- `ipv6_receive` drops unicast not addressed to the namespace; link-local
  destinations are on-link in every namespace; echo replies come from the
  address that was asked.
- Raw `AF_INET6`/`IPPROTO_ICMPV6` sockets with `ICMP6_FILTER`.

## DHCP

- The in-kernel DHCP/DHCPv6 clients serve the initial namespace only; their
  UDP handlers are not consulted for datagrams arriving elsewhere.
- In any namespace BusyBox `udhcpc` works: AF_PACKET for the lease,
  `SO_BROADCAST` (enforced: `EACCES` without it) and `SO_BINDTODEVICE`
  (send and receive by that interface) on its renewing kernel socket.
- Alpine's BusyBox has no `udhcpd`; the smoke uses a small responder built the
  same way.
