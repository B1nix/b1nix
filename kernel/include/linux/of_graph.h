/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_OF_GRAPH_H
#define LKPI_LINUX_OF_GRAPH_H
#include <linux/of.h>

/* An endpoint in a device-tree graph — how an SoC names the link between a
 * display controller and its encoder. x86 has no device tree; the type exists
 * so the core's bridge code compiles. */
struct of_endpoint {
	unsigned int port;
	unsigned int id;
	const struct device_node *local_node;
};
#endif
