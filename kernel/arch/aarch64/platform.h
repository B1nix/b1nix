#ifndef B1NIX_AARCH64_PLATFORM_H
#define B1NIX_AARCH64_PLATFORM_H

#include <b1nix/types.h>

#define PLATFORM_UNKNOWN   0
#define PLATFORM_QEMU_VIRT 1
#define PLATFORM_RPI4      2
#define PLATFORM_SM8150    3

void platform_detect(u64 dtb_addr);
u64 platform_uart_base(void);
u64 platform_aux_base(void);
u64 platform_gicd_base(void);
u64 platform_gicc_base(void);
const char *platform_name(void);
int platform_type(void);
void platform_set_uart_base(u64 base);
void platform_set_aux_base(u64 base);

#endif /* B1NIX_AARCH64_PLATFORM_H */
