#ifndef B1NIX_BCM2835_H
#define B1NIX_BCM2835_H

#include <b1nix/types.h>

/* Property tags this kernel uses. The firmware defines many more. */
#define BCM_TAG_GET_BOARD_REVISION 0x00010002u
#define BCM_TAG_GET_MAC_ADDRESS    0x00010003u
#define BCM_TAG_GET_BOARD_SERIAL   0x00010004u
#define BCM_TAG_GET_ARM_MEMORY     0x00010005u
#define BCM_TAG_ALLOCATE_BUFFER    0x00040001u
#define BCM_TAG_GET_PITCH          0x00040008u
#define BCM_TAG_SET_PHYS_WH        0x00048003u
#define BCM_TAG_SET_VIRT_WH        0x00048004u
#define BCM_TAG_SET_DEPTH          0x00048005u

void bcm2835_mbox_init(void);
void bcm2835_mbox_selftest(void);
int bcm2835_mbox_ready(void);

int bcm2835_property(u32 tag, const u32 *req, u32 req_bytes,
                     u32 *rsp, u32 rsp_bytes);
int bcm2835_board_revision(u32 *revision);
int bcm2835_board_serial(u64 *serial);
int bcm2835_arm_memory(u64 *base, u64 *size);
int bcm2835_mac_address(u8 mac[6]);
int bcm2835_fb_alloc(u32 width, u32 height, u32 depth,
                     u64 *fb_phys, u32 *fb_size, u32 *pitch);

/* GPIO */
void bcm2835_gpio_init(void);
int bcm2835_gpio_ready(void);
#define BCM_GPIO_IN   0u
#define BCM_GPIO_OUT  1u
int bcm2835_gpio_set_function(u32 pin, u32 function);
int bcm2835_gpio_get_function(u32 pin, u32 *function);
int bcm2835_gpio_set(u32 pin, int high);
int bcm2835_gpio_level(u32 pin);
void bcm2835_gpio_selftest(void);

/* The free-running 1 MHz system timer. */
void bcm2835_systimer_init(void);
u64 bcm2835_systimer_us(void);
void bcm2835_systimer_selftest(void);

/* Power management: the only way to reset a Broadcom SoC. */
void bcm2835_pm_init(void);
int bcm2835_pm_ready(void);
void bcm2835_pm_reset(void);
void bcm2835_pm_poweroff(void);

#endif
