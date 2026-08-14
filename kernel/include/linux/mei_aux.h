/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MEI_AUX_H
#define LKPI_LINUX_MEI_AUX_H
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/types.h>
/* The Management Engine auxiliary bus, used for HDCP 2.x and for the GSC on
 * parts newer than this cut targets. No ME interface exists here. */
/* The auxiliary device an ME interface is published as, and the region it
 * describes. No ME here (see above), so nothing is ever registered — the shape
 * exists because a driver's probe names its members. */
#include <linux/auxiliary_bus.h>

struct mei_aux_device {
	struct auxiliary_device aux_dev;
	int irq;
	struct resource bar;
	bool ext_op_mem_supported;
	/* The firmware on this part is slow to answer; the ME driver uses it to
	 * stretch its timeouts. No ME driver here, so nothing reads it. */
	bool slow_firmware;
	struct resource ext_op_mem;
};

#define auxiliary_dev_to_mei_aux_dev(auxdev) \
	container_of(auxdev, struct mei_aux_device, aux_dev)

#endif
