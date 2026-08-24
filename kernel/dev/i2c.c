#include <b1nix/arch.h>
/* /dev/i2c-N — the PIIX4/ICH9 SMBus host controller (M107).
 *
 * QEMU's `pc` machine puts a PIIX4 power-management function at 00:01.3
 * (8086:7113) whose SMBus host controller lives at the I/O base in config
 * register 0x90; the q35 machine has the ICH9 equivalent (8086:2930). This
 * driver probes for either, and if neither is present it registers nothing —
 * there is no software fallback, because a fake SMBus transfer would report a
 * device that is not on the bus.
 *
 * The controller speaks SMBus, not raw I2C: it can issue quick/byte/byte-data/
 * word-data/block transactions but cannot drive an arbitrary I2C message
 * sequence. I2C_FUNCS says so, and I2C_RDWR is refused rather than emulated,
 * which is exactly how Linux behaves on the same silicon.
 */

#include <b1nix/errno.h>
#include <b1nix/i2c.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/pci.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

/* SMBus host-controller register offsets from the I/O base. */
#define SMB_HST_STS  0x00
#define SMB_HST_CNT  0x02
#define SMB_HST_CMD  0x03
#define SMB_XMIT_SLVA 0x04
#define SMB_HST_D0   0x05
#define SMB_HST_D1   0x06
#define SMB_BLOCK_DB 0x07

/* HST_STS bits */
#define SMB_STS_HOST_BUSY 0x01
#define SMB_STS_INTR      0x02
#define SMB_STS_DEV_ERR   0x04
#define SMB_STS_BUS_ERR   0x08
#define SMB_STS_FAILED    0x10
#define SMB_STS_ALL       0x1F

/* HST_CNT: protocol in bits 2-4, START in bit 6. */
#define SMB_CNT_START 0x40
#define SMB_PROTO_QUICK     (0u << 2)
#define SMB_PROTO_BYTE      (1u << 2)
#define SMB_PROTO_BYTE_DATA (2u << 2)
#define SMB_PROTO_WORD_DATA (3u << 2)
#define SMB_PROTO_BLOCK     (5u << 2)

/* PCI config: PIIX4 SMB_IO_BASE and the host configuration register. */
#define PIIX4_SMB_BASE 0x90
#define PIIX4_SMB_HSTCFG 0xD2
#define ICH9_SMB_BASE 0x20
#define ICH9_SMB_HSTCFG 0x40
#define SMB_HSTCFG_ENABLE 0x01

/* Linux <linux/i2c-dev.h> ioctls. */
#define I2C_RETRIES 0x0701
#define I2C_TIMEOUT 0x0702
#define I2C_SLAVE   0x0703
#define I2C_TENBIT  0x0704
#define I2C_FUNCS   0x0705
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR    0x0707
#define I2C_PEC     0x0708
#define I2C_SMBUS   0x0720

/* Function bits (linux/i2c.h). */
#define I2C_FUNC_I2C                    0x00000001
#define I2C_FUNC_SMBUS_QUICK            0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE        0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE       0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA   0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA  0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA   0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA  0x00400000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000

/* i2c_smbus_ioctl_data transaction sizes. */
#define I2C_SMBUS_QUICK      0
#define I2C_SMBUS_BYTE       1
#define I2C_SMBUS_BYTE_DATA  2
#define I2C_SMBUS_WORD_DATA  3
#define I2C_SMBUS_PROC_CALL  4
#define I2C_SMBUS_BLOCK_DATA 5

#define I2C_SMBUS_READ  1
#define I2C_SMBUS_WRITE 0

#define I2C_SMBUS_BLOCK_MAX 32

union i2c_smbus_data_k {
  u8 byte;
  u16 word;
  u8 block[I2C_SMBUS_BLOCK_MAX + 2];
};

struct i2c_smbus_ioctl_data_k {
  u8 read_write;
  u8 command;
  u32 size;
  union i2c_smbus_data_k *data;
};

static u16 g_smb_base;
static int g_present;
static u8 g_slave_addr; /* set by I2C_SLAVE; one bus, one client at a time */
static spinlock_t i2c_lock = SPINLOCK_INIT;

/* Wait for the controller to go idle, then for a transaction to complete.
 *
 * Bounded in time, not in reads: a wedged southbridge must not hang the caller
 * forever, but "a hundred thousand port reads" is a duration only on the
 * machine somebody counted it on — elsewhere it is a different wait entirely,
 * too short to let slow hardware answer or long enough to stall a boot. The
 * clock the kernel calibrates says what a millisecond is. */
static int smb_wait_idle(void) {
  u64 deadline = arch_tsc_monotonic_ns() + 50000000ull; /* 50 ms */

  while (arch_tsc_monotonic_ns() < deadline) {
    u8 s = inb((u16)(g_smb_base + SMB_HST_STS));
    if (!(s & SMB_STS_HOST_BUSY))
      return 0;
  }
  return -EBUSY;
}

static int smb_wait_done(void) {
  u64 start = arch_tsc_monotonic_ns();
  u64 deadline = start + 250000000ull; /* 250 ms */

  while (arch_tsc_monotonic_ns() < deadline) {
    u8 s = inb((u16)(g_smb_base + SMB_HST_STS));
    if (s & (SMB_STS_DEV_ERR | SMB_STS_BUS_ERR | SMB_STS_FAILED)) {
      outb((u16)(g_smb_base + SMB_HST_STS), SMB_STS_ALL);
      /* A device that does not acknowledge is ENXIO, which is what
       * i2cdetect uses to decide a slot is empty. */
      return (s & SMB_STS_DEV_ERR) ? -ENXIO : -EIO;
    }
    if (s & SMB_STS_INTR) {
      outb((u16)(g_smb_base + SMB_HST_STS), SMB_STS_ALL);
      return 0;
    }
    /* Some implementations drop BUSY without raising INTR on a quick command;
     * treat a settled controller with no error as success — but only after
     * giving it a moment to start, or a controller that has not picked the
     * command up yet reads as one that has finished it. The grace used to be
     * "sixteen reads", which is a length of time only on one machine. */
    if (!(s & SMB_STS_HOST_BUSY) &&
        arch_tsc_monotonic_ns() - start > 20000ull /* 20 µs */) {
      outb((u16)(g_smb_base + SMB_HST_STS), SMB_STS_ALL);
      return 0;
    }
  }
  return -ETIMEDOUT;
}

/* One SMBus transaction. `addr` is the 7-bit address. */
static int smb_xfer(u8 addr, int read_write, u8 command, int size,
                    union i2c_smbus_data_k *data) {
  u8 proto;
  u64 flags;
  int rc;

  switch (size) {
  case I2C_SMBUS_QUICK:
    proto = SMB_PROTO_QUICK;
    break;
  case I2C_SMBUS_BYTE:
    proto = SMB_PROTO_BYTE;
    break;
  case I2C_SMBUS_BYTE_DATA:
    proto = SMB_PROTO_BYTE_DATA;
    break;
  case I2C_SMBUS_WORD_DATA:
    proto = SMB_PROTO_WORD_DATA;
    break;
  case I2C_SMBUS_BLOCK_DATA:
    proto = SMB_PROTO_BLOCK;
    break;
  default:
    return -EOPNOTSUPP;
  }

  spin_lock_irqsave(&i2c_lock, &flags);
  outb((u16)(g_smb_base + SMB_HST_STS), SMB_STS_ALL);
  rc = smb_wait_idle();
  if (rc < 0) {
    spin_unlock_irqrestore(&i2c_lock, flags);
    return rc;
  }

  outb((u16)(g_smb_base + SMB_XMIT_SLVA),
       (u8)((addr << 1) | (read_write == I2C_SMBUS_READ ? 1 : 0)));
  if (size != I2C_SMBUS_QUICK)
    outb((u16)(g_smb_base + SMB_HST_CMD), command);
  if (read_write == I2C_SMBUS_WRITE) {
    if (size == I2C_SMBUS_BYTE_DATA)
      outb((u16)(g_smb_base + SMB_HST_D0), data->byte);
    else if (size == I2C_SMBUS_WORD_DATA) {
      outb((u16)(g_smb_base + SMB_HST_D0), (u8)(data->word & 0xFF));
      outb((u16)(g_smb_base + SMB_HST_D1), (u8)(data->word >> 8));
    } else if (size == I2C_SMBUS_BLOCK_DATA) {
      u8 len = data->block[0];
      if (len == 0 || len > I2C_SMBUS_BLOCK_MAX) {
        spin_unlock_irqrestore(&i2c_lock, flags);
        return -EINVAL;
      }
      outb((u16)(g_smb_base + SMB_HST_D0), len);
      inb((u16)(g_smb_base + SMB_HST_CNT)); /* reset the block pointer */
      for (u8 i = 0; i < len; i++)
        outb((u16)(g_smb_base + SMB_BLOCK_DB), data->block[i + 1]);
    }
  } else if (size == I2C_SMBUS_BLOCK_DATA) {
    inb((u16)(g_smb_base + SMB_HST_CNT));
  }

  outb((u16)(g_smb_base + SMB_HST_CNT), (u8)(proto | SMB_CNT_START));
  rc = smb_wait_done();
  if (rc == 0 && read_write == I2C_SMBUS_READ) {
    if (size == I2C_SMBUS_BYTE || size == I2C_SMBUS_BYTE_DATA)
      data->byte = inb((u16)(g_smb_base + SMB_HST_D0));
    else if (size == I2C_SMBUS_WORD_DATA)
      data->word = (u16)(inb((u16)(g_smb_base + SMB_HST_D0)) |
                         ((u16)inb((u16)(g_smb_base + SMB_HST_D1)) << 8));
    else if (size == I2C_SMBUS_BLOCK_DATA) {
      u8 len = inb((u16)(g_smb_base + SMB_HST_D0));
      if (len > I2C_SMBUS_BLOCK_MAX)
        len = I2C_SMBUS_BLOCK_MAX;
      data->block[0] = len;
      for (u8 i = 0; i < len; i++)
        data->block[i + 1] = inb((u16)(g_smb_base + SMB_BLOCK_DB));
    }
  }
  spin_unlock_irqrestore(&i2c_lock, flags);
  return rc;
}

static int i2c_ioctl(struct vfs_node *node, u64 request, void *arg) {
  (void)node;
  if (!g_present)
    return -ENXIO;
  switch (request) {
  case I2C_SLAVE:
  case I2C_SLAVE_FORCE: {
    unsigned long a = (unsigned long)(usize)arg;
    if (a > 0x7F)
      return -EINVAL;
    g_slave_addr = (u8)a;
    return 0;
  }
  case I2C_TENBIT:
    /* The host controller only drives 7-bit addressing. */
    return (usize)arg ? -EOPNOTSUPP : 0;
  case I2C_RETRIES:
  case I2C_TIMEOUT:
    return 0; /* the transaction poll is already bounded */
  case I2C_PEC:
    return (usize)arg ? -EOPNOTSUPP : 0;
  case I2C_FUNCS: {
    unsigned long funcs =
        I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_READ_BYTE |
        I2C_FUNC_SMBUS_WRITE_BYTE | I2C_FUNC_SMBUS_READ_BYTE_DATA |
        I2C_FUNC_SMBUS_WRITE_BYTE_DATA | I2C_FUNC_SMBUS_READ_WORD_DATA |
        I2C_FUNC_SMBUS_WRITE_WORD_DATA | I2C_FUNC_SMBUS_READ_BLOCK_DATA |
        I2C_FUNC_SMBUS_WRITE_BLOCK_DATA;
    /* Deliberately no I2C_FUNC_I2C: this is an SMBus host controller and
     * cannot issue arbitrary I2C messages. i2ctransfer will say so. */
    if (!arg || syscall_copyout(arg, &funcs, sizeof(funcs)) < 0)
      return -EFAULT;
    return 0;
  }
  case I2C_RDWR:
    return -EOPNOTSUPP;
  case I2C_SMBUS: {
    struct i2c_smbus_ioctl_data_k req;
    if (!arg || syscall_copyin(&req, arg, sizeof(req)) < 0)
      return -EFAULT;
    union i2c_smbus_data_k data;
    memset(&data, 0, sizeof(data));
    int has_data = req.size != I2C_SMBUS_QUICK;
    if (has_data && req.read_write == I2C_SMBUS_WRITE) {
      if (!req.data || syscall_copyin(&data, req.data, sizeof(data)) < 0)
        return -EFAULT;
    }
    int rc = smb_xfer(g_slave_addr, req.read_write, req.command, (int)req.size,
                      &data);
    if (rc < 0)
      return rc;
    if (has_data && req.read_write == I2C_SMBUS_READ) {
      if (!req.data || syscall_copyout(req.data, &data, sizeof(data)) < 0)
        return -EFAULT;
    }
    return 0;
  }
  default:
    return -ENOTTY;
  }
}

void i2c_register_nodes(void) {
  if (!g_present)
    return;
  struct vfs_node *n = vfs_add_node("/dev/i2c-0", VFS_DEVICE, 0, 0, 0);
  if (!n || IS_ERR(n))
    return;
  n->inode->mode = 0600;
  n->inode->ioctl_cb = i2c_ioctl;
}

void i2c_init(void) {
  struct pci_device_info pci;
  u8 base_reg, cfg_reg;

  if (pci_find_device(0x8086, 0x7113, &pci)) { /* PIIX4 ACPI/SMBus function */
    base_reg = PIIX4_SMB_BASE;
    cfg_reg = PIIX4_SMB_HSTCFG;
  } else if (pci_find_device(0x8086, 0x2930, &pci)) { /* ICH9 SMBus */
    base_reg = ICH9_SMB_BASE;
    cfg_reg = ICH9_SMB_HSTCFG;
  } else {
    return; /* no controller: register nothing */
  }

  u32 base = pci_config_read32(pci.bus, pci.slot, pci.func, base_reg);
  base &= 0xFFFEu;
  if (base == 0)
    return;

  u8 hstcfg = pci_config_read8(pci.bus, pci.slot, pci.func, cfg_reg);
  if (!(hstcfg & SMB_HSTCFG_ENABLE))
    pci_config_write8(pci.bus, pci.slot, pci.func, cfg_reg,
                      (u8)(hstcfg | SMB_HSTCFG_ENABLE));

  g_smb_base = (u16)base;
  g_present = 1;

  char msg[80];
  snprintf(msg, sizeof(msg), "i2c: SMBus host controller at I/O 0x%x\n",
           (unsigned)g_smb_base);
  console_write(msg);

  i2c_register_nodes();
}
