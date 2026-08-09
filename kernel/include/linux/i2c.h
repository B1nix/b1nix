/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_I2C_H
#define LKPI_LINUX_I2C_H
#include <b1nix/i2c.h>
#include <linux/device.h>
/* Onto b1nix's i2c. DRM uses it for DDC — reading an EDID off a monitor — so
 * this is one of the few here that a display driver genuinely exercises. */
struct i2c_adapter { struct device dev; const char *name; void *algo_data; };
struct i2c_msg { u16 addr; u16 flags; u16 len; u8 *buf; };
#define I2C_M_RD 0x0001
/* An i2c client and its driver, for encoders that hang off the bus. Nothing
 * probes them here — see i2c_transfer, which reports the bus as absent — so
 * registration succeeds and binds nothing. */
struct i2c_device_id { char name[20]; unsigned long driver_data; };
struct i2c_client { struct device dev; struct i2c_adapter *adapter; u16 addr; };
struct i2c_driver {
	int (*probe)(struct i2c_client *);
	void (*remove)(struct i2c_client *);
	struct device_driver driver;
	const struct i2c_device_id *id_table;
};
/* Instantiating a client from a board description. Nothing describes a board
 * here — no device tree, no ACPI methods — so nothing is instantiated. */
struct i2c_board_info { char type[20]; u16 addr; void *platform_data; };
#define I2C_BOARD_INFO(dev_type, dev_addr) .type = dev_type, .addr = (dev_addr)
#define I2C_MODULE_PREFIX "i2c:"
static inline struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap,
                                                       const struct i2c_board_info *info)
{ (void)adap; (void)info; return 0; }
static inline void i2c_unregister_device(struct i2c_client *c) { (void)c; }
static inline struct i2c_adapter *i2c_get_adapter(int nr)
{ (void)nr; return 0; }
static inline void i2c_put_adapter(struct i2c_adapter *a) { (void)a; }

static inline int i2c_register_driver(struct module *owner,
                                      struct i2c_driver *drv)
{ (void)owner; (void)drv; return 0; }
static inline void i2c_del_driver(struct i2c_driver *drv) { (void)drv; }
static inline void *i2c_get_clientdata(const struct i2c_client *c)
{ return c ? dev_get_drvdata(&c->dev) : 0; }
static inline void i2c_set_clientdata(struct i2c_client *c, void *d)
{ if (c) dev_set_drvdata(&c->dev, d); }

#define to_i2c_driver(d) container_of(d, struct i2c_driver, driver)
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)
static inline bool i2c_client_has_driver(struct i2c_client *c)
{ return c && c->dev.driver; }

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
#endif
