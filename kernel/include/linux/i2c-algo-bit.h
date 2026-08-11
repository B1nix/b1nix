/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_I2C_ALGO_BIT_H
#define LKPI_LINUX_I2C_ALGO_BIT_H
#include <linux/i2c.h>
/*
 * Bit-banged I2C: the GMBUS fallback i915 uses to read EDID when the hardware
 * controller will not talk. The algorithm is upstream's and lives in
 * i2c-algo-bit.c, which is GPL-2.0 and therefore not imported; what is here is
 * the data the driver fills in. A driver that falls back to bit-banging will
 * fail to link rather than silently read no EDID — GMBUS is the path that has
 * to work.
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
