/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI battery, AC adapter and thermal zones (M134).
 *
 * These are the three things the M129 roadmap said were blocked on an AML
 * interpreter, and this file is what the interpreter unblocked. It contains
 * no hardware knowledge at all: every value is the result of evaluating a
 * method the firmware compiled into its DSDT.
 *
 *   battery   _STA (bit 4 = present), _BIF or _BIX (the fixed information:
 *             units, design and last-full capacity), _BST (the live state:
 *             charging/discharging, rate, remaining capacity, voltage)
 *   adapter   _PSR (1 when the mains are connected)
 *   thermal   _TMP (tenths of a kelvin)
 *
 * A machine whose firmware declares none of them publishes nothing. That is
 * not a gap: a QEMU guest genuinely has no battery, and a /sys tree that
 * showed one would be lying about the hardware.
 */

#include <b1nix/acpi_power.h>
#include <b1nix/aml.h>
#include <b1nix/kprintf.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <stdio.h>
#include <string.h>

#define PS_PATH_MAX 96

struct ps_dev {
    char path[PS_PATH_MAX];
    int  energy_units;      /* battery only: 0 = mAh, 1 = mWh */
};

static struct ps_dev g_bat[ACPI_PS_MAX_BATTERY];
static struct ps_dev g_ac[ACPI_PS_MAX_AC];
static struct ps_dev g_tz[ACPI_PS_MAX_THERMAL];
static int g_nbat, g_nac, g_ntz;
static int g_scanned;

/* The 32-bit form of a seven-character EISA id, the way AML stores it: three
 * five-bit letters packed big-endian into the first two bytes, then the four
 * hex digits one per nibble. Computed rather than written out so a new device
 * class is one string, not a magic number. */
static u32 eisa_id(const char *s) {
    if (strlen(s) != 7)
        return 0;
    u16 mfg = (u16)(((s[0] - 0x40) & 0x1F) << 10 |
                    ((s[1] - 0x40) & 0x1F) << 5 |
                    ((s[2] - 0x40) & 0x1F));
    u8 d[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 2; k++) {
            char ch = s[3 + i * 2 + k];
            u8 v = 0;
            if (ch >= '0' && ch <= '9')      v = (u8)(ch - '0');
            else if (ch >= 'A' && ch <= 'F') v = (u8)(ch - 'A' + 10);
            else if (ch >= 'a' && ch <= 'f') v = (u8)(ch - 'a' + 10);
            d[i] = (u8)((d[i] << 4) | v);
        }
    }
    /* Little-endian dword of the bytes { mfg_hi, mfg_lo, d0, d1 }. */
    return (u32)((mfg >> 8) & 0xFF) | ((u32)(mfg & 0xFF) << 8) |
           ((u32)d[0] << 16) | ((u32)d[1] << 24);
}

static void path_join(char *out, usize cap, const char *base, const char *leaf) {
    snprintf(out, cap, "%s.%s", base, leaf);
}

/* Does the device at `base` answer to this _HID or _CID? Both the integer
 * (EISA) and the string forms occur in real firmware. */
static int hid_matches(const char *base, const char *want) {
    static const char *const keys[] = { "_HID", "_CID" };
    u32 eid = eisa_id(want);
    for (usize k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        char p[PS_PATH_MAX + 8];
        struct aml_result r;
        path_join(p, sizeof(p), base, keys[k]);
        if (aml_evaluate(p, 0, 0, &r) != AML_OK)
            continue;
        if (r.type == AML_T_INTEGER && eid && (u32)r.integer == eid)
            return 1;
        if (r.type == AML_T_STRING && r.bytes_copied >= 7 &&
            memcmp(r.bytes, want, 7) == 0)
            return 1;
        /* A _CID may be a package of ids. */
        if (r.type == AML_T_PACKAGE) {
            for (u32 i = 0; i < r.elems; i++)
                if (r.elem_type[i] == AML_T_INTEGER && eid &&
                    (u32)r.elem_int[i] == eid)
                    return 1;
        }
    }
    return 0;
}

/* The walk callback may not evaluate anything: aml_walk holds the
 * interpreter's lock for the length of the walk, and aml_evaluate takes the
 * same lock. So the walk only COLLECTS candidates, and the questions are
 * asked afterwards, with the lock free. */
struct scan_ctx {
    char (*dev)[PS_PATH_MAX];
    char (*tz)[PS_PATH_MAX];
    int ndev, ntz, maxdev, maxtz;
};

static void scan_one(void *ctx, const char *path, int type) {
    struct scan_ctx *sc = (struct scan_ctx *)ctx;
    if (strlen(path) >= PS_PATH_MAX)
        return;
    if (type == AML_T_DEVICE && sc->ndev < sc->maxdev)
        strncpy(sc->dev[sc->ndev++], path, PS_PATH_MAX - 1);
    else if (type == AML_T_THERMAL && sc->ntz < sc->maxtz)
        strncpy(sc->tz[sc->ntz++], path, PS_PATH_MAX - 1);
}

/* ACPI0003 is eight characters, so eisa_id() cannot encode it; firmware
 * spells it as a string. hid_matches handles both, and eisa_id returns 0 for
 * anything that is not seven characters, which the integer test then skips. */

static int eval_pkg(const char *base, const char *method,
                    struct aml_result *r) {
    char p[PS_PATH_MAX + 8];
    path_join(p, sizeof(p), base, method);
    if (aml_evaluate(p, 0, 0, r) != AML_OK)
        return -1;
    if (r->type != AML_T_PACKAGE)
        return -1;
    return 0;
}

static int eval_int(const char *base, const char *method, u64 *out) {
    char p[PS_PATH_MAX + 8];
    struct aml_result r;
    path_join(p, sizeof(p), base, method);
    if (aml_evaluate(p, 0, 0, &r) != AML_OK)
        return -1;
    if (r.type != AML_T_INTEGER)
        return -1;
    *out = r.integer;
    return 0;
}

#define PS_SCAN_MAX_DEV 256
#define PS_SCAN_MAX_TZ  32

void acpi_power_init(void) {
    if (g_scanned || !aml_ready() || aml_table_count() == 0)
        return;
    g_scanned = 1;

    struct scan_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.dev = kzalloc(PS_SCAN_MAX_DEV * PS_PATH_MAX);
    sc.tz = kzalloc(PS_SCAN_MAX_TZ * PS_PATH_MAX);
    if (!sc.dev || !sc.tz) {
        kfree(sc.dev);
        kfree(sc.tz);
        return;
    }
    sc.maxdev = PS_SCAN_MAX_DEV;
    sc.maxtz = PS_SCAN_MAX_TZ;
    aml_walk(scan_one, &sc);

    for (int i = 0; i < sc.ntz && g_ntz < ACPI_PS_MAX_THERMAL; i++) {
        char p[PS_PATH_MAX + 8];
        path_join(p, sizeof(p), sc.tz[i], "_TMP");
        /* A zone with no _TMP has no temperature to report. */
        if (aml_exists(p))
            strncpy(g_tz[g_ntz++].path, sc.tz[i], PS_PATH_MAX - 1);
    }
    for (int i = 0; i < sc.ndev; i++) {
        char p[PS_PATH_MAX + 8];
        if (g_nbat < ACPI_PS_MAX_BATTERY && hid_matches(sc.dev[i], "PNP0C0A")) {
            path_join(p, sizeof(p), sc.dev[i], "_BST");
            if (aml_exists(p))
                strncpy(g_bat[g_nbat++].path, sc.dev[i], PS_PATH_MAX - 1);
            continue;
        }
        if (g_nac < ACPI_PS_MAX_AC && hid_matches(sc.dev[i], "ACPI0003")) {
            path_join(p, sizeof(p), sc.dev[i], "_PSR");
            if (aml_exists(p))
                strncpy(g_ac[g_nac++].path, sc.dev[i], PS_PATH_MAX - 1);
        }
    }
    kfree(sc.dev);
    kfree(sc.tz);

    /* The unit the firmware reports in is _BIF element 0: 0 means mW/mWh
     * (energy), 1 means mA/mAh (charge). Read once — it does not change. */
    for (int i = 0; i < g_nbat; i++) {
        struct aml_result r;
        g_bat[i].energy_units = 1;
        if (eval_pkg(g_bat[i].path, "_BIF", &r) == 0 && r.elems >= 1 &&
            r.elem_type[0] == AML_T_INTEGER)
            g_bat[i].energy_units = r.elem_int[0] == 0 ? 1 : 0;
        k_info("acpi", "battery %s (%s units)", g_bat[i].path,
               g_bat[i].energy_units ? "energy" : "charge");
    }
    for (int i = 0; i < g_nac; i++)
        k_info("acpi", "ac adapter %s", g_ac[i].path);
    for (int i = 0; i < g_ntz; i++)
        k_info("acpi", "thermal zone %s", g_tz[i].path);
}

int acpi_power_battery_count(void) { return g_nbat; }
int acpi_power_ac_count(void)      { return g_nac; }
int acpi_power_thermal_count(void) { return g_ntz; }

int acpi_power_battery_in_energy_units(int idx) {
    if (idx < 0 || idx >= g_nbat)
        return 1;
    return g_bat[idx].energy_units;
}

const char *acpi_power_battery_path(int idx) {
    return (idx >= 0 && idx < g_nbat) ? g_bat[idx].path : "";
}
const char *acpi_power_ac_path(int idx) {
    return (idx >= 0 && idx < g_nac) ? g_ac[idx].path : "";
}
const char *acpi_power_thermal_path(int idx) {
    return (idx >= 0 && idx < g_ntz) ? g_tz[idx].path : "";
}

/* _BST element 0 is a bit mask: 1 discharging, 2 charging, 4 critical. */
#define BST_DISCHARGING 1u
#define BST_CHARGING    2u

int acpi_power_battery_attr(int idx, int which, char *buf, usize cap) {
    if (idx < 0 || idx >= g_nbat)
        return -1;
    const char *base = g_bat[idx].path;

    if (which == ACPI_BAT_TYPE)
        return snprintf(buf, cap, "Battery\n");

    if (which == ACPI_BAT_PRESENT) {
        u64 sta = 0;
        char p[PS_PATH_MAX + 8];
        path_join(p, sizeof(p), base, "_STA");
        if (!aml_exists(p))
            return snprintf(buf, cap, "1\n");   /* no _STA means always there */
        if (eval_int(base, "_STA", &sta) < 0)
            return -1;
        return snprintf(buf, cap, "%u\n", (sta & 0x10) ? 1u : 0u);
    }

    struct aml_result bst;
    if (eval_pkg(base, "_BST", &bst) < 0 || bst.elems < 4)
        return -1;
    u64 state = bst.elem_int[0];
    u64 remaining = bst.elem_int[2];

    if (which == ACPI_BAT_STATUS) {
        const char *s;
        if (state & BST_DISCHARGING)   s = "Discharging";
        else if (state & BST_CHARGING) s = "Charging";
        else                           s = "Full";
        return snprintf(buf, cap, "%s\n", s);
    }

    struct aml_result bif;
    if (eval_pkg(base, "_BIF", &bif) < 0 || bif.elems < 3)
        return -1;
    u64 full = bif.elem_int[2];             /* last full charge capacity */
    if (full == 0 || full == 0xFFFFFFFFULL)
        full = bif.elem_int[1];             /* fall back to the design value */

    switch (which) {
    case ACPI_BAT_CAPACITY:
        if (!full || remaining == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(remaining * 100 / full));
    case ACPI_BAT_NOW:
        if (remaining == 0xFFFFFFFFULL)
            return -1;
        /* ACPI reports mWh or mAh; sysfs is µWh or µAh. */
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(remaining * 1000));
    case ACPI_BAT_FULL:
        if (!full)
            return -1;
        return snprintf(buf, cap, "%llu\n", (unsigned long long)(full * 1000));
    default:
        return -1;
    }
}

int acpi_power_ac_attr(int idx, char *buf, usize cap) {
    if (idx < 0 || idx >= g_nac)
        return -1;
    u64 on = 0;
    if (eval_int(g_ac[idx].path, "_PSR", &on) < 0)
        return -1;
    return snprintf(buf, cap, "%u\n", on ? 1u : 0u);
}

int acpi_power_thermal_attr(int idx, int which, char *buf, usize cap) {
    if (idx < 0 || idx >= g_ntz)
        return -1;
    if (which == ACPI_TZ_TYPE) {
        /* Linux calls the zone by its ACPI path; the last segment is what a
         * user sees in `sensors`. */
        const char *p = g_tz[idx].path;
        const char *last = p;
        for (const char *q = p; *q; q++)
            if (*q == '.' || *q == '\\')
                last = q + 1;
        return snprintf(buf, cap, "%s\n", last);
    }
    u64 dk = 0;
    if (eval_int(g_tz[idx].path, "_TMP", &dk) < 0)
        return -1;
    /* Tenths of a kelvin to thousandths of a degree Celsius. A zone reading
     * below absolute zero is a firmware that did not answer, not a cold
     * laptop, so it is refused rather than reported as a negative. */
    if (dk < 2732)
        return -1;
    long long milli = (long long)dk * 100 - 273150;
    return snprintf(buf, cap, "%lld\n", milli);
}
