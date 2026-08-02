#ifndef B1NIX_I2C_H
#define B1NIX_I2C_H

#include <b1nix/types.h>

/*
 * SMBus host controller (PIIX4 / ICH9) exposed as /dev/i2c-0 — M107.
 * kernel/dev/i2c.c. i2c_init() probes PCI; if no controller is present it
 * registers no device node at all rather than pretending to have a bus.
 */
void i2c_init(void);
void i2c_register_nodes(void);

#endif
