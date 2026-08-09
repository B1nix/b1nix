/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SPI_SPI_H
#define LKPI_LINUX_SPI_SPI_H
#include <linux/device.h>
/* SPI, used by small panel controllers. No M102 target has one. */
struct spi_device { struct device dev; };
#endif
