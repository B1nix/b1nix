/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PCI_IDS_H
#define LKPI_LINUX_PCI_IDS_H
/* The vendor and subsystem ids drivers match on. Only the ones imported code
 * names are here — a full table would be copied data nobody reads. */
#define PCI_VENDOR_ID_INTEL              0x8086
#define PCI_VENDOR_ID_REDHAT_QUMRANET    0x1af4
#define PCI_SUBVENDOR_ID_REDHAT_QUMRANET 0x1af4
#define PCI_SUBDEVICE_ID_QEMU            0x1100

/* PCI base classes (the top byte of the class code), which are the spec's. */
#define PCI_BASE_CLASS_DISPLAY 0x03

#define PCI_CLASS_BRIDGE_HOST 0x0600

#endif
