#ifndef B1NIX_PLATFORM_H
#define B1NIX_PLATFORM_H

#include <b1nix/types.h>

#define PLATFORM_UNKNOWN   0
#define PLATFORM_QEMU_VIRT 1
#define PLATFORM_RPI4      2
#define PLATFORM_SM8150    3

#if defined(__aarch64__)
void platform_detect(u64 dtb_addr);
u64 platform_uart_base(void);
u64 platform_aux_base(void);
u64 platform_gicd_base(void);
u64 platform_gicc_base(void);
const char *platform_name(void);
int platform_type(void);
void platform_set_uart_base(u64 base);
void platform_set_aux_base(u64 base);
#else
static inline void platform_detect(u64 dtb_addr) { (void)dtb_addr; }
static inline u64 platform_uart_base(void) { return 0; }
static inline u64 platform_aux_base(void) { return 0; }
static inline u64 platform_gicd_base(void) { return 0; }
static inline u64 platform_gicc_base(void) { return 0; }
static inline const char *platform_name(void) { return "PC"; }
static inline int platform_type(void) { return PLATFORM_UNKNOWN; }
static inline void platform_set_uart_base(u64 base) { (void)base; }
static inline void platform_set_aux_base(u64 base) { (void)base; }
#endif

#endif /* B1NIX_PLATFORM_H */
