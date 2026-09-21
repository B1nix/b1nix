/* SPDX-License-Identifier: GPL-2.0-only */
/* NUMA topology: which memory belongs to which node, which CPUs sit beside it,
 * and how far apart the nodes are (M128). Built from ACPI's SRAT and SLIT.
 *
 * A machine without an SRAT — every QEMU guest started without `-numa`, and
 * most real desktops — is one node covering all of memory, and every query
 * here answers accordingly. Nothing above this layer needs to ask whether the
 * machine is NUMA; it asks which node, and gets 0. */
#ifndef B1NIX_NUMA_H
#define B1NIX_NUMA_H

#include <b1nix/types.h>

/* Linux builds with 64 or 1024; the tables here are static, and a machine with
 * more nodes than this reports the ones that fit and says so in the log. */
#define NUMA_MAX_NODES 8

/* Distances are SLIT's units: 10 is "the same node", and Linux's tools print
 * these numbers verbatim in `numactl --hardware`. */
#define NUMA_DISTANCE_LOCAL   10
#define NUMA_DISTANCE_DEFAULT 20

/* Parse SRAT and SLIT. Safe to call once, after the ACPI tables are reachable
 * (acpi_init) and the direct map exists. */
void numa_init(void);

/* 1 when a real SRAT described more than one node. Everything below works
 * either way; this is for the places that must not claim a topology the
 * firmware never gave (sysfs's `has_cpu`, the boot log). */
int numa_present(void);

/* The number of nodes the machine has, at least 1. */
int numa_node_count(void);

/* The node a physical frame belongs to. O(1): a byte per chunk of RAM. */
int numa_node_of_frame(u64 frame);

/* The node a CPU sits on, by this kernel's CPU index or by APIC id. */
int numa_node_of_cpu(int cpu);
int numa_node_of_apic(u32 apic_id);

/* The node the CPU this code is running on belongs to. */
int numa_node_here(void);

/* SLIT's distance between two nodes, or 10/20 when the firmware gave none. */
int numa_distance(int from, int to);

/* The CPUs of a node, as a bitmap of this kernel's CPU indices. */
u64 numa_cpumask(int node);

/* The bytes of RAM SRAT gave the node (what it is made of, not what is free —
 * that is the allocator's answer, see pmm_node_free_frames). */
u64 numa_node_bytes(int node);

/* Nodes ordered by distance from `pref`, nearest first, `pref` itself at [0].
 * Fills `out` with numa_node_count() entries and returns that count. This is
 * the fallback order an allocation walks when its own node is empty. */
int numa_node_order(int pref, int *out);

#endif
