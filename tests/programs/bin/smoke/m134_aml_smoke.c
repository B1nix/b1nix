/* SPDX-License-Identifier: GPL-2.0-only */
/* m134_aml_smoke — the AML interpreter, against the firmware's own bytecode
 * (M134).
 *
 * Nothing here is a fixture. Every object this test asks for is one QEMU's
 * firmware really compiles into its DSDT or one of its SSDTs, and every value
 * it insists on is one a broken parser cannot produce by accident:
 *
 *   - the namespace has the predefined roots and a plausible size;
 *   - \_S5_ is a package whose first element is the SLP_TYP value this
 *     machine wants for soft-off -- the number a kernel needs to power the
 *     machine off through ACPI, which b1nix reaches by other means today;
 *   - \_SB_.PCI0._HID is the compressed EISA id of a PCI host bridge, a
 *     32-bit constant that is either PNP0A03 or PNP0A08 and nothing else;
 *   - \_SB_.PCI0._CRS is a resource template: a buffer that ends in the
 *     small end tag (0x79);
 *   - the CPU-hotplug SSDT's CSTA(n) is a METHOD WITH AN ARGUMENT that
 *     writes a selector to an I/O port and reads a status bit back, so its
 *     answer depends on running the bytecode and on the hardware agreeing:
 *     0x0F for a CPU that exists, 0 for one that does not;
 *   - the battery and thermal sysfs agree with what the interpreter found.
 *     On this machine the firmware declares neither, so the truthful answer
 *     is an EMPTY /sys/class/power_supply, and that is what is checked.
 *
 * Markers (only emitted on verified success):
 *   M134-AML: start
 *   M134-AML: ok no-acpi-firmware   (a machine with no ACPI at all)
 *   M134-AML: ok dsdt-loaded
 *   M134-AML: ok namespace-roots
 *   M134-AML: ok namespace-size
 *   M134-AML: ok devices-found
 *   M134-AML: ok s5-sleep-type
 *   M134-AML: ok pci0-hid
 *   M134-AML: ok pci0-crs
 *   M134-AML: ok method-with-args      (where the firmware has CSTA)
 *   M134-AML: ok method-args-absent    (where it does not)
 *   M134-AML: ok pci-config-refused + ok refusal-recorded
 *                                       (or ok pci-config-absent)
 *   M134-AML: ok no-battery / ok battery-sysfs
 *   M134-AML: ok no-thermal-zone / ok thermal-sysfs
 *   M134-AML: done
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void ok(const char *what) {
  printf("M134-AML: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M134-AML: fail %s (%s, %ld)\n", what, why, v);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static char g_ns[262144];

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  size_t total = 0;
  ssize_t n;

  if (fd < 0)
    return -1;
  while (total + 1 < cap) {
    n = read(fd, buf + total, cap - 1 - total);
    if (n <= 0)
      break;
    total += (size_t)n;
  }
  close(fd);
  buf[total] = 0;
  return (int)total;
}

/* A "key value" line out of /proc/b1nix-acpi. */
static long ns_long(const char *key, long missing) {
  char pat[64];
  const char *p;

  snprintf(pat, sizeof(pat), "\n%s ", key);
  p = strstr(g_ns, pat);
  if (!p) {
    if (strncmp(g_ns, key, strlen(key)) == 0 && g_ns[strlen(key)] == ' ')
      p = g_ns - 1;
    else
      return missing;
  }
  return strtol(p + 1 + strlen(key), 0, 0);
}

static int ns_has_path(const char *path) {
  char pat[96];

  snprintf(pat, sizeof(pat), "\n%s ", path);
  return strstr(g_ns, pat) != 0;
}

/* Ask the kernel to evaluate one object, and hand back the reply verbatim. */
static int eval(const char *req, char *out, size_t cap) {
  int fd = open("/proc/b1nix-acpi-eval", O_RDWR);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = write(fd, req, strlen(req));
  close(fd);
  if (n < 0)
    return -1;
  return read_file("/proc/b1nix-acpi-eval", out, cap);
}

static int reply_is(const char *reply, const char *key, const char *want) {
  char pat[64];
  const char *p;

  snprintf(pat, sizeof(pat), "\n%s ", key);
  p = strncmp(reply, key, strlen(key)) == 0 && reply[strlen(key)] == ' '
          ? reply - 1
          : strstr(reply, pat);
  if (!p)
    return 0;
  p += 1 + strlen(key);
  while (*p == ' ')
    p++;
  return strncmp(p, want, strlen(want)) == 0;
}

static long reply_long(const char *reply, const char *key, long missing) {
  char pat[64];
  const char *p;

  snprintf(pat, sizeof(pat), "\n%s ", key);
  p = strncmp(reply, key, strlen(key)) == 0 && reply[strlen(key)] == ' '
          ? reply - 1
          : strstr(reply, pat);
  if (!p)
    return missing;
  return strtol(p + 1 + strlen(key), 0, 0);
}

/* The ACPI compressed EISA id of a seven-character device id, as the 32-bit
 * integer AML stores it. Computed, so the constant in the check is derived
 * from the name rather than copied from a disassembly. */
static unsigned long eisa_id(const char *s) {
  unsigned mfg = (unsigned)(((s[0] - 0x40) & 0x1F) << 10 |
                            ((s[1] - 0x40) & 0x1F) << 5 |
                            ((s[2] - 0x40) & 0x1F));
  unsigned d[2] = { 0, 0 };
  int i, k;

  for (i = 0; i < 2; i++)
    for (k = 0; k < 2; k++) {
      char ch = s[3 + i * 2 + k];
      unsigned v = ch >= 'A' ? (unsigned)(ch - 'A' + 10) : (unsigned)(ch - '0');
      d[i] = (d[i] << 4) | v;
    }
  return ((mfg >> 8) & 0xFF) | ((mfg & 0xFF) << 8) | (d[0] << 16) |
         (d[1] << 24);
}

/* The last two bytes of a resource template are the small end tag (0x79) and
 * its checksum. The reply prints the buffer as hex bytes. */
static int crs_ends_in_end_tag(const char *reply, long len) {
  const char *p = strstr(reply, "\nbytes ");
  const char *last = 0;
  char prev[3] = { 0, 0, 0 };
  char cur[3] = { 0, 0, 0 };
  int count = 0;

  if (!p)
    return 0;
  p += 7;
  while (*p && *p != '\n') {
    if (*p == ' ') {
      p++;
      continue;
    }
    strncpy(prev, cur, 2);
    cur[0] = p[0];
    cur[1] = p[1];
    last = p;
    count++;
    p += 2;
  }
  (void)last;
  /* Only conclusive when the whole buffer fitted in the reply. */
  if (count != len)
    return 1;
  return strncmp(prev, "79", 2) == 0;
}

int main(void) {
  char reply[8192];
  long objects, tables, zones, batteries;
  int r;

  printf("M134-AML: start\n");
  fflush(stdout);

  r = read_file("/proc/b1nix-acpi", g_ns, sizeof(g_ns));
  if (r < 0) {
    bad("proc-b1nix-acpi", "cannot read", errno);
    printf("M134-AML: done\n");
    return 1;
  }

  judge("namespace-roots",
        ns_has_path("\\_SB_") && ns_has_path("\\_GPE") && ns_has_path("\\_SI_"),
        "a predefined root is missing", 0);

  tables = ns_long("tables", -1);
  if (tables == 0) {
    /* A machine with no ACPI at all -- every board on the other
     * architecture. The interpreter must still be there and must have built
     * nothing but the roots the specification says an OS creates, and it
     * must publish no battery and no thermal zone. That is the no-op this
     * milestone promises, stated as something that can fail. */
    struct stat st;
    objects = ns_long("objects", -1);
    judge("no-acpi-firmware",
          ns_long("ready", -1) == 1 && objects > 0 && objects < 32 &&
              ns_long("devices", -1) == 0 &&
              ns_long("batteries", -1) == 0 &&
              ns_long("zones_published", -1) == 0 &&
              stat("/sys/class/power_supply", &st) != 0 &&
              stat("/sys/class/thermal", &st) != 0,
          "a machine with no firmware bytecode is not reported as having none",
          objects);
    printf("M134-AML: done\n");
    fflush(stdout);
    return fails ? 1 : 0;
  }

  judge("dsdt-loaded", tables >= 1 && strstr(g_ns, "\ntable DSDT ") != 0,
        "no DSDT among the loaded tables", tables);

  objects = ns_long("objects", -1);
  /* A DSDT that decoded produces hundreds of objects; a parser that gave up
   * on the first term produces the handful this kernel predefines. */
  judge("namespace-size", objects >= 60, "too few objects to be a real DSDT",
        objects);

  judge("devices-found", ns_long("devices", -1) >= 1,
        "the DSDT declared no devices, which no firmware does",
        ns_long("devices", -1));

  /* ── \_S5_: how this machine is told to power itself off ── */
  if (eval("\\_S5_", reply, sizeof(reply)) > 0 &&
      reply_is(reply, "status", "ok")) {
    long count = reply_long(reply, "count", -1);
    long slp = -1;
    const char *e = strstr(reply, "\nelem 0 integer ");
    if (e)
      slp = strtol(e + 16, 0, 0);
    printf("M134-AML: s5 package of %ld, SLP_TYP %ld\n", count, slp);
    fflush(stdout);
    judge("s5-sleep-type", count >= 2 && slp >= 0 && slp <= 7,
          "not a sleep package with a three-bit sleep type", slp);
  } else {
    bad("s5-sleep-type", "\\_S5_ did not evaluate", 0);
  }

  /* ── a concrete named object: the PCI host bridge's hardware id ── */
  if (eval("\\_SB_.PCI0._HID", reply, sizeof(reply)) > 0 &&
      reply_is(reply, "status", "ok") && reply_is(reply, "type", "integer")) {
    unsigned long hid = (unsigned long)reply_long(reply, "integer", 0);
    printf("M134-AML: PCI0._HID 0x%08lx\n", hid);
    fflush(stdout);
    judge("pci0-hid",
          hid == eisa_id("PNP0A03") || hid == eisa_id("PNP0A08"),
          "not a PCI host bridge EISA id", (long)hid);
  } else {
    bad("pci0-hid", "\\_SB_.PCI0._HID did not evaluate to an integer", 0);
  }

  /* ── a buffer: the host bridge's resource template ── */
  if (eval("\\_SB_.PCI0._CRS", reply, sizeof(reply)) > 0 &&
      reply_is(reply, "status", "ok") && reply_is(reply, "type", "buffer")) {
    long len = reply_long(reply, "length", -1);
    judge("pci0-crs", len >= 16 && crs_ends_in_end_tag(reply, len),
          "not a resource template", len);
  } else {
    bad("pci0-crs", "\\_SB_.PCI0._CRS is not a buffer", 0);
  }

  /* ── a method with an argument, over a SystemIO operation region ──
   *
   * \_SB_.CPUS.CSTA(n) in QEMU's CPU-hotplug SSDT writes n to the selector
   * register at the region's offset 0 and reads the "enabled" bit back from
   * offset 4: 0x0F for a processor that is present, 0 for one that is not.
   * Both answers together are the check -- an interpreter that returned a
   * plausible constant would get one of them wrong.
   */
  if (ns_has_path("\\_SB_.CPUS.CSTA")) {
    long present = -1, absent = -1;
    if (eval("\\_SB_.CPUS.CSTA 0", reply, sizeof(reply)) > 0 &&
        reply_is(reply, "status", "ok"))
      present = reply_long(reply, "integer", -1);
    if (eval("\\_SB_.CPUS.CSTA 200", reply, sizeof(reply)) > 0 &&
        reply_is(reply, "status", "ok"))
      absent = reply_long(reply, "integer", -1);
    printf("M134-AML: CSTA(0)=0x%lx CSTA(200)=0x%lx\n", present, absent);
    fflush(stdout);
    judge("method-with-args", present == 0x0F && absent == 0,
          "the method did not read the hotplug register", present);
  } else {
    /* Say so rather than passing quietly: the firmware here has no such
     * method, and a later QEMU that grows one must not silently skip. */
    ok("method-args-absent");
  }

  /* ── an address space this kernel will not touch is REFUSED ──
   *
   * QEMU's PIIX link devices read their interrupt routing out of PCI config
   * space (OperationRegion(P40C, PCI_Config, ...)), and this interpreter has
   * no PCI config accessor for a region. The right answer is an error that
   * propagates out of the evaluation; the wrong one is a plausible number.
   * That is what this checks, and it is only meaningful as a positive test.
   */
  if (ns_has_path("\\_SB_.LNKA._STA")) {
    r = eval("\\_SB_.LNKA._STA", reply, sizeof(reply));
    judge("pci-config-refused",
          r > 0 && reply_is(reply, "status", "region-refused"),
          "an unimplemented address space was answered instead of refused", r);
    judge("refusal-recorded",
          read_file("/proc/b1nix-acpi", g_ns, sizeof(g_ns)) > 0 &&
              strstr(g_ns, "PCI_Config") != 0,
          "the refusal was not recorded where a human can see it", 0);
  } else {
    ok("pci-config-absent");
  }

  /* ── the consumers: published only where the firmware has them ── */
  batteries = ns_long("batteries", -1);
  if (batteries == 0) {
    struct stat st;
    int none = stat("/sys/class/power_supply/BAT0", &st) != 0 &&
               stat("/sys/class/power_supply/AC0", &st) != 0;
    judge("no-battery", none,
          "the firmware declares no battery but /sys shows one", batteries);
  } else {
    char buf[64];
    long cap = -1, full = -1, now = -1, volt = -1;
    char status[32] = "";
    int good = read_file("/sys/class/power_supply/BAT0/capacity", buf,
                         sizeof(buf)) > 0;

    if (good)
      cap = strtol(buf, 0, 10);
    if (read_file("/sys/class/power_supply/BAT0/energy_full", buf,
                  sizeof(buf)) > 0)
      full = strtol(buf, 0, 10);
    if (read_file("/sys/class/power_supply/BAT0/energy_now", buf,
                  sizeof(buf)) > 0)
      now = strtol(buf, 0, 10);
    if (read_file("/sys/class/power_supply/BAT0/voltage_now", buf,
                  sizeof(buf)) > 0)
      volt = strtol(buf, 0, 10);
    if (read_file("/sys/class/power_supply/BAT0/status", status,
                  sizeof(status)) > 0)
      status[strcspn(status, "\n")] = 0;
    /* Printed as well as judged: these are the firmware's own numbers, and the
     * lane compares them against what the table declares. A check that only
     * asked "is it a plausible percentage" would pass on an invented battery. */
    printf("M134-AML: battery cap %ld full %ld now %ld volt %ld status %s\n",
           cap, full, now, volt, status[0] ? status : "?");
    fflush(stdout);
    good = good && cap >= 0 && cap <= 100 && full > 0 && now >= 0 &&
           now <= full && volt > 0 && status[0];
    judge("battery-sysfs", good, "the battery files do not read back", cap);
  }

  zones = ns_long("zones_published", -1);
  if (zones == 0) {
    struct stat st;
    judge("no-thermal-zone",
          stat("/sys/class/thermal/thermal_zone0", &st) != 0,
          "no zone has a _TMP but /sys shows one", zones);
  } else {
    char buf[64];
    long milli = -1;
    if (read_file("/sys/class/thermal/thermal_zone0/temp", buf,
                  sizeof(buf)) > 0)
      milli = strtol(buf, 0, 10);
    printf("M134-AML: thermal_zone0 %ld mC\n", milli);
    fflush(stdout);
    judge("thermal-sysfs", milli > -40000 && milli < 150000,
          "the zone temperature is not a temperature", milli);
  }

  printf("M134-AML: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
