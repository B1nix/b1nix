#ifndef B1NIX_WATCHDOG_H
#define B1NIX_WATCHDOG_H

#include <b1nix/types.h>

/* /dev/watchdog — a software watchdog with the WDIOC_* interface (M107).
 * kernel/dev/watchdog.c. */
void watchdog_init(void);
void watchdog_register_nodes(void);
/* Deadline check, called once per scheduler tick on the boot CPU. */
void watchdog_tick(void);

#endif
