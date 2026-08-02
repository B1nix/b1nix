#ifndef B1NIX_NETCONSOLE_H
#define B1NIX_NETCONSOLE_H

#include <b1nix/types.h>

/*
 * M98 T1 — kernel log over UDP.
 *
 * tests/smoke.sh reads COM1 and nothing else, which is fine under QEMU but
 * useless on the bring-up laptop, which has no serial port at all. netconsole
 * ships the klog ring to a UDP collector so bare-metal boots have a log
 * channel.
 *
 * Configured entirely from the kernel command line:
 *
 *     b1nix.netconsole=<ip>:<port>
 *
 * Draining happens in a kernel thread, never inside console_write(). That is
 * not a stylistic choice: console_write() takes console_lock with interrupts
 * off and runs from interrupt context, so a synchronous send would re-enter the
 * NIC driver underneath the console lock and deadlock the moment the driver
 * logged anything. The kthread reads the ring through the cursor API and calls
 * udp_send() in a clean, preemptible context.
 */

/* Parse the command line and, when a target is configured, start the drain
 * thread. Safe to call when networking is unavailable — the thread simply has
 * nothing to send until the stack comes up. Idempotent. */
void netconsole_init(void);

/* 1 when a target was configured and the drain thread is running. */
int netconsole_active(void);

/* Datagrams successfully handed to the UDP layer since boot. */
u64 netconsole_packets_sent(void);

/* Push whatever is pending right now, from the caller's context. Used by the
 * self-test; also the shape a future panic-path flush would take. Returns the
 * number of datagrams sent. */
int netconsole_flush(void);

/* M98 in-kernel self-test: configures a loopback target, emits a unique line
 * into the klog ring, drains it through the real UDP stack and verifies the
 * bytes arrive at a registered handler. Emits M98-DRV-SMOKE markers. No-op
 * outside b1nix.test=1. */
void netconsole_selftest(void);

#endif
