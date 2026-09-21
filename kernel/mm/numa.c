/* SPDX-License-Identifier: GPL-2.0-only */
/* NUMA topology from ACPI (M128).
 *
 * SRAT says which physical ranges and which CPUs belong to which proximity
 * domain; SLIT says how far apart those domains are. Both are optional, and a
 * machine without them is one node — which is what every lane but the `numa`
 * one is, so the fast paths below have to cost nothing there.
 *
 * The one query on a hot path is numa_node_of_frame: the allocator asks it for
 * every block it links or unlinks. It is a byte per chunk of RAM (64 MiB), so
 * the answer is one shift and one load, and the table for a 1 TiB machine is
 * 16 KiB. Proximity domains are renumbered into dense node ids as they are
 * met, because that is what userspace expects to see under
 * /sys/devices/system/node. */

#include <b1nix/numa.h>

#if defined(__x86_64__)
#include <b1nix/acpi.h>
#endif
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>

#include <string.h>

/* One byte of node table per this many bytes of RAM. A range boundary that
 * does not land on a chunk is rounded so that the node covering most of the
 * chunk wins; firmware ranges are aligned far more coarsely than this in
 * practice (QEMU splits at gibibytes). */
#define NUMA_CHUNK_SHIFT 26 /* 64 MiB */
#define NUMA_CHUNK_BYTES (1ULL << NUMA_CHUNK_SHIFT)

struct numa_node {
  u32 domain;    /* the proximity domain SRAT used */
  u64 bytes;     /* RAM SRAT gave it */
  u64 cpumask;   /* this kernel's CPU indices */
  u8 used;
};

static struct numa_node g_nodes[NUMA_MAX_NODES];
static int g_node_count = 1; /* one node until SRAT says otherwise */
static int g_numa_present;

/* node id per chunk of physical memory; 0 (node 0) until numa_init fills it */
static u8 *g_chunk_node;
static usize g_chunk_count;

/* SLIT, dense-indexed like the nodes. 0 means "the firmware did not say". */
static u8 g_distance[NUMA_MAX_NODES][NUMA_MAX_NODES];

/* apic id → node, for the CPUs SRAT named. 0xff = not named. */
#define NUMA_APIC_SLOTS 256
static u8 g_apic_node[NUMA_APIC_SLOTS];

#if defined(__x86_64__)
/* ── the ACPI tables ─────────────────────────────────────────────────────── */

struct acpi_srat {
  struct acpi_sdt_header header;
  u32 reserved1; /* 1 */
  u64 reserved2;
} __attribute__((packed));

#define SRAT_TYPE_CPU_AFFINITY    0
#define SRAT_TYPE_MEM_AFFINITY    1
#define SRAT_TYPE_X2APIC_AFFINITY 2

struct srat_cpu_affinity {
  u8 type;
  u8 length;
  u8 domain_lo;
  u8 apic_id;
  u32 flags;
  u8 sapic_eid;
  u8 domain_hi[3];
  u32 clock_domain;
} __attribute__((packed));

struct srat_mem_affinity {
  u8 type;
  u8 length;
  u32 domain;
  u16 reserved1;
  u32 base_lo;
  u32 base_hi;
  u32 len_lo;
  u32 len_hi;
  u32 reserved2;
  u32 flags;
  u64 reserved3;
} __attribute__((packed));

struct srat_x2apic_affinity {
  u8 type;
  u8 length;
  u16 reserved1;
  u32 domain;
  u32 x2apic_id;
  u32 flags;
  u32 clock_domain;
  u32 reserved2;
} __attribute__((packed));

#define SRAT_ENABLED (1u << 0)

struct acpi_slit {
  struct acpi_sdt_header header;
  u64 locality_count;
  /* u8 entry[locality_count * locality_count] follows */
} __attribute__((packed));

/* ── node ids ────────────────────────────────────────────────────────────── */

/* The dense id for a proximity domain, creating one the first time it is met.
 * Returns -1 when the machine has more nodes than this kernel carries. */
static int node_for_domain(u32 domain) {
  for (int i = 0; i < NUMA_MAX_NODES; i++)
    if (g_nodes[i].used && g_nodes[i].domain == domain)
      return i;
  for (int i = 0; i < NUMA_MAX_NODES; i++)
    if (!g_nodes[i].used) {
      g_nodes[i].used = 1;
      g_nodes[i].domain = domain;
      return i;
    }
  return -1;
}

static void chunk_fill(u64 base, u64 length, int node) {
  if (!g_chunk_node || length == 0)
    return;

  u64 first = base >> NUMA_CHUNK_SHIFT;
  u64 last = (base + length - 1) >> NUMA_CHUNK_SHIFT;

  for (u64 c = first; c <= last && c < (u64)g_chunk_count; c++)
    g_chunk_node[c] = (u8)node;
}

static void parse_srat(const struct acpi_srat *srat) {
  const u8 *p = (const u8 *)srat + sizeof(*srat);
  const u8 *end = (const u8 *)srat + srat->header.length;

  while (p + 2 <= end) {
    u8 type = p[0];
    u8 len = p[1];

    if (len < 2 || p + len > end)
      break;
    if (type == SRAT_TYPE_MEM_AFFINITY && len >= sizeof(struct srat_mem_affinity)) {
      const struct srat_mem_affinity *m = (const struct srat_mem_affinity *)p;

      if (m->flags & SRAT_ENABLED) {
        u64 base = ((u64)m->base_hi << 32) | m->base_lo;
        u64 length = ((u64)m->len_hi << 32) | m->len_lo;
        int node = node_for_domain(m->domain);

        if (node >= 0 && length) {
          g_nodes[node].bytes += length;
          chunk_fill(base, length, node);
        }
      }
    } else if (type == SRAT_TYPE_CPU_AFFINITY &&
               len >= sizeof(struct srat_cpu_affinity)) {
      const struct srat_cpu_affinity *c = (const struct srat_cpu_affinity *)p;

      if (c->flags & SRAT_ENABLED) {
        u32 domain = (u32)c->domain_lo | ((u32)c->domain_hi[0] << 8) |
                     ((u32)c->domain_hi[1] << 16) |
                     ((u32)c->domain_hi[2] << 24);
        int node = node_for_domain(domain);

        if (node >= 0)
          g_apic_node[c->apic_id] = (u8)node;
      }
    } else if (type == SRAT_TYPE_X2APIC_AFFINITY &&
               len >= sizeof(struct srat_x2apic_affinity)) {
      const struct srat_x2apic_affinity *x =
          (const struct srat_x2apic_affinity *)p;

      if ((x->flags & SRAT_ENABLED) && x->x2apic_id < NUMA_APIC_SLOTS) {
        int node = node_for_domain(x->domain);

        if (node >= 0)
          g_apic_node[x->x2apic_id] = (u8)node;
      }
    }
    p += len;
  }
}

static void parse_slit(const struct acpi_slit *slit) {
  u64 n = slit->locality_count;
  const u8 *m = (const u8 *)slit + sizeof(*slit);

  if (n == 0 || n > 1024)
    return;
  if (sizeof(*slit) + n * n > slit->header.length)
    return;
  /* SLIT is indexed by proximity domain, the dense ids are ours: translate
   * through the domain each node was created from. A domain SRAT never
   * mentioned has no node here and is skipped. */
  for (int a = 0; a < g_node_count; a++)
    for (int b = 0; b < g_node_count; b++) {
      u64 da = g_nodes[a].domain;
      u64 db = g_nodes[b].domain;

      if (da < n && db < n)
        g_distance[a][b] = m[da * n + db];
    }
}

#endif /* __x86_64__ */

/* ── init ────────────────────────────────────────────────────────────────── */

void numa_init(void) {
#if !defined(__x86_64__)
  /* SRAT and SLIT are ACPI, and the aarch64 port has no ACPI: the boards it
   * runs on describe themselves in a device tree, whose numa-node-id
   * properties are a separate parser nobody needs yet (QEMU virt and every
   * phone here are one node). One node, and every accessor already says so. */
  return;
#else
  const struct acpi_sdt_header *srat, *slit;
  u64 ram;

  memset(g_apic_node, 0xff, sizeof(g_apic_node));
  srat = acpi_find_table("SRAT");
  if (!srat)
    return; /* one node, and every accessor already answers that way */

  /* The chunk table covers the machine's RAM, not the direct map's window:
   * a range SRAT names above what this kernel can map still belongs to its
   * node, and a lookup beyond the table answers node 0. */
  ram = pmm_phys_total_memory();
  if (!ram)
    ram = pmm_total_usable_memory();
  g_chunk_count = (usize)((ram + NUMA_CHUNK_BYTES - 1) >> NUMA_CHUNK_SHIFT) + 1;
  g_chunk_node = kzalloc(g_chunk_count);
  if (!g_chunk_node) {
    g_chunk_count = 0;
    console_write("numa: no memory for the node table; treating the machine as one node\n");
    return;
  }

  parse_srat((const struct acpi_srat *)srat);

  g_node_count = 0;
  for (int i = 0; i < NUMA_MAX_NODES; i++)
    if (g_nodes[i].used)
      g_node_count = i + 1;
  if (g_node_count == 0)
    g_node_count = 1;

  /* Distances default to "local or far" and are replaced by SLIT's numbers
   * where it has them. */
  for (int a = 0; a < NUMA_MAX_NODES; a++)
    for (int b = 0; b < NUMA_MAX_NODES; b++)
      g_distance[a][b] = (a == b) ? NUMA_DISTANCE_LOCAL : NUMA_DISTANCE_DEFAULT;
  slit = acpi_find_table("SLIT");
  if (slit)
    parse_slit((const struct acpi_slit *)slit);

  g_numa_present = g_node_count > 1;
  if (g_numa_present) {
    console_write("numa: ");
    console_write_dec((u64)g_node_count);
    console_write(" nodes from SRAT");
    for (int i = 0; i < g_node_count; i++) {
      console_write(", node ");
      console_write_dec((u64)i);
      console_write(" ");
      console_write_dec(g_nodes[i].bytes >> 20);
      console_write(" MiB");
    }
    console_write("\n");
  }
#endif
}

/* ── queries ─────────────────────────────────────────────────────────────── */

int numa_present(void) { return g_numa_present; }

int numa_node_count(void) { return g_node_count < 1 ? 1 : g_node_count; }

int numa_node_of_frame(u64 frame) {
  usize chunk = (usize)(frame >> NUMA_CHUNK_SHIFT);

  if (!g_chunk_node || chunk >= g_chunk_count)
    return 0;
  return g_chunk_node[chunk];
}

int numa_node_of_apic(u32 apic_id) {
  if (apic_id >= NUMA_APIC_SLOTS || g_apic_node[apic_id] == 0xff)
    return 0;
  return g_apic_node[apic_id];
}

int numa_node_of_cpu(int cpu) {
  struct percpu *pc;

  if (!g_numa_present || cpu < 0 || cpu >= MAX_CPUS)
    return 0;
  pc = get_percpu_n(cpu);
  if (!pc)
    return 0;
  return numa_node_of_apic(pc->apic_id);
}

int numa_node_here(void) {
  struct percpu *pc;

  if (!g_numa_present)
    return 0;
  pc = get_percpu();
  return pc ? numa_node_of_apic(pc->apic_id) : 0;
}

int numa_distance(int from, int to) {
  if (from < 0 || to < 0 || from >= NUMA_MAX_NODES || to >= NUMA_MAX_NODES)
    return NUMA_DISTANCE_DEFAULT;
  if (!g_distance[from][to])
    return from == to ? NUMA_DISTANCE_LOCAL : NUMA_DISTANCE_DEFAULT;
  return g_distance[from][to];
}

u64 numa_cpumask(int node) {
  u64 mask = 0;

  if (node < 0 || node >= numa_node_count())
    return 0;
  if (!g_numa_present)
    node = 0;
  for (int cpu = 0; cpu < g_max_cpus && cpu < MAX_CPUS; cpu++) {
    struct percpu *pc = get_percpu_n(cpu);

    if (!pc)
      continue;
    if (numa_node_of_apic(pc->apic_id) == node)
      mask |= 1ULL << cpu;
  }
  /* A single-node machine's CPUs all belong to node 0 whether or not any of
   * them is online yet: an empty cpulist would make `numactl --hardware`
   * report a node nothing runs on. */
  if (!mask && node == 0)
    for (int cpu = 0; cpu < (g_max_cpus > 0 ? g_max_cpus : 1); cpu++)
      mask |= 1ULL << cpu;
  return mask;
}

u64 numa_node_bytes(int node) {
  if (node < 0 || node >= numa_node_count())
    return 0;
  if (!g_numa_present)
    return pmm_total_usable_memory();
  return g_nodes[node].bytes;
}

int numa_node_order(int pref, int *out) {
  int n = numa_node_count();
  int count = 0;
  u8 taken[NUMA_MAX_NODES] = {0};

  if (!out)
    return 0;
  if (pref < 0 || pref >= n)
    pref = 0;
  out[count++] = pref;
  taken[pref] = 1;
  /* Selection sort by distance: n is 8 at most, and this runs where an
   * allocation has already failed on its own node. */
  while (count < n) {
    int best = -1;

    for (int i = 0; i < n; i++) {
      if (taken[i])
        continue;
      if (best < 0 || numa_distance(pref, i) < numa_distance(pref, best))
        best = i;
    }
    if (best < 0)
      break;
    taken[best] = 1;
    out[count++] = best;
  }
  return count;
}
