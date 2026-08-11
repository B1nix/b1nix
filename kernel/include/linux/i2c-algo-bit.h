/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_I2C_ALGO_BIT_H
#define LKPI_LINUX_I2C_ALGO_BIT_H
#include <linux/i2c.h>
/*
 * Bit-banged I2C: the GMBUS fallback i915 uses to read EDID when the hardware
 * controller will not talk. This is the data the driver fills in; the protocol
 * that drives it is kernel/lkpi/i2c_bit.c, written from the specification
 * because upstream's i2c-algo-bit.c is GPL-2.0 and not imported.
 */
struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *data, int state);
	void (*setscl)(void *data, int state);
	int (*getsda)(void *data);
	int (*getscl)(void *data);
	int (*pre_xfer)(struct i2c_adapter *);
	void (*post_xfer)(struct i2c_adapter *);
	int udelay;
	int timeout;
};

int i2c_bit_add_bus(struct i2c_adapter *adap);
#endif
