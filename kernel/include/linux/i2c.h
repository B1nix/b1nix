/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_I2C_H
#define LKPI_LINUX_I2C_H
#include <b1nix/i2c.h>
#include <linux/device.h>
/* Onto b1nix's i2c. DRM uses it for DDC — reading an EDID off a monitor — so
 * this is one of the few here that a display driver genuinely exercises. */
struct i2c_lock_operations;
struct i2c_algorithm;

struct i2c_adapter {
	struct device dev;
	const char *name;
	void *algo_data;
	/* Optional cross-transfer bus locking; see the note further down. NULL
	 * here, so the core's per-transfer serialisation is what applies. */
	const struct i2c_lock_operations *lock_ops;
	/* How transfers are actually issued. */
	const struct i2c_algorithm *algo;
	int retries;
	int timeout;
	/* The module that owns the adapter. Nothing here is a module, so it is
	 * recorded and never dereferenced. */
	struct module *owner;
	unsigned int class;
};
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

/* Message flags. NOSTART matters for the GMBUS fallback: a continued read must
 * not re-issue the start condition, and a controller that did would restart the
 * transfer rather than continue it. */
#define I2C_M_RD       0x0001
#define I2C_M_TEN      0x0010
#define I2C_M_NOSTART  0x4000
#define I2C_M_STOP     0x8000
#define I2C_M_IGNORE_NAK 0x1000


/* The bus-locking operations an adapter publishes so a client can hold the bus
 * across several transfers — i915 needs it to keep GMBUS for a full EDID read.
 * b1nix's i2c core serialises each transfer and has no cross-transfer hold, so
 * an adapter here leaves this NULL and the core's own locking applies. */
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *, unsigned int flags);
};

#define I2C_LOCK_ROOT_ADAPTER 0
#define I2C_LOCK_SEGMENT      1


/* The transfer algorithm an adapter is driven by. GMBUS supplies its own;
 * i2c_bit_algo is the bit-banged fallback, which is GPL-2.0 upstream and
 * therefore not imported — see <linux/i2c-algo-bit.h>. Declared so an adapter
 * that names it fails to link rather than silently reading no EDID. */
struct i2c_algorithm {
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	u32 (*functionality)(struct i2c_adapter *adap);
};

extern const struct i2c_algorithm i2c_bit_algo;


/* Registering and removing an adapter. b1nix's i2c core owns the buses it
 * knows about; an adapter added here is recorded so the driver's own transfers
 * route through it, and removing one drops that registration. */
int i2c_add_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);
int i2c_check_functionality(struct i2c_adapter *adap, u32 func);

#define I2C_FUNC_I2C            0x00000001
#define I2C_FUNC_SMBUS_EMUL     0x0eff0008
#define I2C_CLASS_DDC           (1 << 3)


#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x02000000
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL  0x00008000
#define I2C_FUNC_10BIT_ADDR             0x00000002
#define I2C_FUNC_NOSTART                0x00000010


/* The lock-free transfer: the caller already holds the bus. Same path as
 * i2c_transfer here, because b1nix's core takes no bus lock of its own — see
 * the note on i2c_lock_operations. */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);


/* The adapter and client name field width, as userspace sees it in sysfs. */
#define I2C_NAME_SIZE 20

#endif
