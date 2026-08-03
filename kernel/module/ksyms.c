/* Kernel symbols exported to loadable modules (M95).
 *
 * This is the module ABI: every name here is resolvable from a .ko, nothing
 * else is. Keeping the whole table in one translation unit makes the surface
 * auditable — `grep EXPORT_SYMBOL kernel/module/ksyms.c` is the exhaustive
 * list, and tools/kernel/check-module-syms.sh diffs each built module's
 * undefined symbols against it so a missing export is a build failure rather
 * than an insmod failure.
 */

#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/mixer.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/netproto.h>
#include <b1nix/panic.h>
#include <b1nix/pci.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/sound.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── libc-ish primitives ─────────────────────────────────────────────────── */
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(snprintf);
EXPORT_SYMBOL(atoi);

/* Compiler-emitted 64-bit divide helpers. */
u64 __udivdi3(u64 num, u64 den);
u64 __umoddi3(u64 num, u64 den);
EXPORT_SYMBOL(__udivdi3);
EXPORT_SYMBOL(__umoddi3);

/* ── console / diagnostics ───────────────────────────────────────────────── */
EXPORT_SYMBOL(console_write);
EXPORT_SYMBOL(console_putc);
EXPORT_SYMBOL(console_write_dec);
EXPORT_SYMBOL(console_write_hex32);
EXPORT_SYMBOL(console_write_hex64);
EXPORT_SYMBOL(panic);
EXPORT_SYMBOL(bootinfo_has_flag);

/* ── memory ──────────────────────────────────────────────────────────────── */
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kzalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(pmm_alloc_frame);
EXPORT_SYMBOL(pmm_alloc_frames);
EXPORT_SYMBOL(pmm_free_frame);
EXPORT_SYMBOL(vmm_map_page);
EXPORT_SYMBOL(vmm_unmap_page);
EXPORT_SYMBOL(vmm_map_mmio);
EXPORT_SYMBOL(vmm_direct_map_base);
EXPORT_SYMBOL(vmm_virt_to_phys);

/* ── scheduler / time ────────────────────────────────────────────────────── */
EXPORT_SYMBOL(scheduler_yield);
EXPORT_SYMBOL(scheduler_sleep_ticks);
EXPORT_SYMBOL(scheduler_get_uptime_ticks);
EXPORT_SYMBOL(rtc_now_unix_seconds);
EXPORT_SYMBOL(rtc_set_unix_time);

/* ── VFS ─────────────────────────────────────────────────────────────────── */
EXPORT_SYMBOL(vfs_register_fs);
EXPORT_SYMBOL(vfs_unregister_fs);
EXPORT_SYMBOL(vfs_create_node);
EXPORT_SYMBOL(vfs_add_node);
EXPORT_SYMBOL(vfs_find_node);
EXPORT_SYMBOL(vfs_node_get);
EXPORT_SYMBOL(vfs_node_put);
EXPORT_SYMBOL(vfs_attach_child);
EXPORT_SYMBOL(vfs_detach_child);
EXPORT_SYMBOL(vfs_set_currently_mounting_root);
EXPORT_SYMBOL(vfs_mount);
EXPORT_SYMBOL(vfs_umount);
EXPORT_SYMBOL(vfs_open);
EXPORT_SYMBOL(vfs_read);
EXPORT_SYMBOL(vfs_close);

/* ── block layer ─────────────────────────────────────────────────────────── */
EXPORT_SYMBOL(blk_get);
EXPORT_SYMBOL(blk_at);
EXPORT_SYMBOL(blk_read_cached);

/* ── PCI / port I/O ──────────────────────────────────────────────────────── */
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_config_read16);
EXPORT_SYMBOL(pci_config_read32);
EXPORT_SYMBOL(pci_config_write16);
EXPORT_SYMBOL(inb);
EXPORT_SYMBOL(outb);

/* ── sound core ──────────────────────────────────────────────────────────── */
EXPORT_SYMBOL(sound_register);
EXPORT_SYMBOL(sound_unregister);
EXPORT_SYMBOL(sound_get_default);
EXPORT_SYMBOL(sound_mixer_ioctl);
EXPORT_SYMBOL(sound_register_hooks);
EXPORT_SYMBOL(sound_unregister_hooks);

/* ── networking core ─────────────────────────────────────────────────────── */
EXPORT_SYMBOL(proto_register);
EXPORT_SYMBOL(proto_unregister);
EXPORT_SYMBOL(proto_find);
EXPORT_SYMBOL(ndp_dispatch_receive);
EXPORT_SYMBOL(ndp_dispatch_resolve);
EXPORT_SYMBOL(net_proto_ipv6_send);
EXPORT_SYMBOL(net_is_ready);
EXPORT_SYMBOL(net_poll);
EXPORT_SYMBOL(net_send_ethernet);
/* M84 routing: the IPv6 and NDP modules pick an egress interface and a
   next hop through the FIB, so the routing entry points are part of the
   module ABI too. */
EXPORT_SYMBOL(net_send_ethernet_dev);
EXPORT_SYMBOL(netdev_by_index);
/* M107: ndp.ko's neighbour dump reports the interface each entry belongs to. */
EXPORT_SYMBOL(netdev_active);
EXPORT_SYMBOL(netdev_index_of);
EXPORT_SYMBOL(route_flow_hash);
EXPORT_SYMBOL(route6_lookup_flow);
EXPORT_SYMBOL(route6_configure_interface);
/* DHCPv6 stays built into the kernel; ndp.ko starts it once a router
   advertisement asks for stateful configuration. */
EXPORT_SYMBOL(dhcpv6_init);
EXPORT_SYMBOL(dhcpv6_start);
EXPORT_SYMBOL(net_loopback_enqueue);
EXPORT_SYMBOL(net_loopback_drain);
EXPORT_SYMBOL(net_get_mac);
EXPORT_SYMBOL(net_get_ip6);
EXPORT_SYMBOL(net_get_ip6_ll);
EXPORT_SYMBOL(net_get_gateway6);
EXPORT_SYMBOL(net_get_prefix6);
EXPORT_SYMBOL(net_get_prefix6_valid);
EXPORT_SYMBOL(net_set_ip6);
EXPORT_SYMBOL(net_set_gateway6);
EXPORT_SYMBOL(net_set_prefix6);
EXPORT_SYMBOL(tcp6_receive);
EXPORT_SYMBOL(udp6_receive);
EXPORT_SYMBOL(udp_register_handler);
EXPORT_SYMBOL(udp_unregister_handler);
EXPORT_SYMBOL(udp_send_net);
EXPORT_SYMBOL(dns_resolve_sync_quiet);

/* ── module framework itself ─────────────────────────────────────────────── */
EXPORT_SYMBOL(request_module);
EXPORT_SYMBOL(try_module_get);
EXPORT_SYMBOL(module_put);
EXPORT_SYMBOL(module_find);
EXPORT_SYMBOL(module_symbol_lookup);
