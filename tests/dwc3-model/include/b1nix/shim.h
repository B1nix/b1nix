/* SPDX-License-Identifier: GPL-2.0-only */
/* Just enough of the kernel for kernel/dev/dwc3_gadget.c to build on the host. */
#ifndef DWC3_MODEL_SHIM_H
#define DWC3_MODEL_SHIM_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t i32; typedef size_t usize;
typedef volatile int spinlock_t;
struct mac_addr { u8 bytes[6]; };
struct ipv4_addr { u8 bytes[4]; };
struct netdev {
	const char *name; char ifname[16]; struct mac_addr mac; int irq;
	int (*transmit)(struct netdev *, const u8 hdr[14], const void *, usize, u32);
	void (*poll)(struct netdev *);
	int (*link_up)(struct netdev *);
	int (*irq_ack)(struct netdev *);
};
#define NET_RX_F_CSUM_OK 1u
#define VMM_WRITABLE 1u
#define BOOTMARK(n) ((void)(n))
void spin_lock_irqsave(spinlock_t *l, u64 *flags);
void spin_unlock_irqrestore(spinlock_t *l, u64 flags);
void console_write(const char *s);
void console_write_dec(u64 v);
void console_write_hex64(u64 v);
int bootinfo_has_flag(const char *f);
int bootinfo_get_kv(const char *k, char *buf, usize len);
u64 fdt_dwc3_base(void); u64 fdt_qcom_gcc_base(void); u64 fdt_qcom_hsphy_base(void);
void *vmm_map_mmio(u64 phys, usize len, u32 flags);
u64 vmm_direct_map_base(void);
void net_set_ip(struct ipv4_addr ip);
void net_set_netmask(struct ipv4_addr m);
void route_configure_interface(struct ipv4_addr ip, struct ipv4_addr m, struct ipv4_addr gw);
void ethernet_receive_flags(const u8 *frame, usize len, u32 flags);
int netdev_register(struct netdev *nd);
void irq_unmask(int irq);
void arch_udelay(u32 us);
#endif
