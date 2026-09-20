/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_VIRTIO_CONSOLE_H
#define B1NIX_VIRTIO_CONSOLE_H

#include <b1nix/types.h>

/* virtio-console (/dev/hvc0). See kernel/dev/virtio_console.c. */
void virtio_console_init(void);
int virtio_console_ready(void);
/* Queue text for the host; never sleeps, safe under the console lock. */
void virtio_console_write(const char *buf, usize len);
/* Timer tick: reclaim transmit buffers, collect input. */
void virtio_console_poll(void);

#endif
