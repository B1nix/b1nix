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
    u64  alarm;             /* battery only: the _BTP trip last set, milli- */
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

/* A battery's static information, from _BIX (ACPI 4.0 and later) where the
 * firmware has it and _BIF otherwise. The two carry the same numbers at
 * different offsets -- _BIX puts a revision first -- and _BIX adds the cycle
 * count. `str` is where the four strings begin: model, serial, type, OEM. */
struct bat_info {
    u64 unit, design, full, tech, design_mv, warn, low;
    i64 cycles;          /* -1: _BIF has none */
    const char *method;  /* "_BIX" or "_BIF" */
    u32 str;
};

static int bat_info(int idx, struct bat_info *bi) {
    struct aml_result r;
    const char *base = g_bat[idx].path;

    memset(bi, 0, sizeof(*bi));
    if (eval_pkg(base, "_BIX", &r) == 0 && r.elems >= 20) {
        bi->method = "_BIX";
        bi->unit = r.elem_int[1];
        bi->design = r.elem_int[2];
        bi->full = r.elem_int[3];
        bi->tech = r.elem_int[4];
        bi->design_mv = r.elem_int[5];
        bi->warn = r.elem_int[6];
        bi->low = r.elem_int[7];
        bi->cycles = r.elem_int[8] == 0xFFFFFFFFULL ? -1 : (i64)r.elem_int[8];
        bi->str = 16;
        return 0;
    }
    if (eval_pkg(base, "_BIF", &r) == 0 && r.elems >= 13) {
        bi->method = "_BIF";
        bi->unit = r.elem_int[0];
        bi->design = r.elem_int[1];
        bi->full = r.elem_int[2];
        bi->tech = r.elem_int[3];
        bi->design_mv = r.elem_int[4];
        bi->warn = r.elem_int[5];
        bi->low = r.elem_int[6];
        bi->cycles = -1;
        bi->str = 9;
        return 0;
    }
    return -1;
}

/* One of the four strings, trimmed of trailing blanks and NULs. */
static int bat_string(int idx, const struct bat_info *bi, u32 which,
                      char *out, usize cap) {
    char p[PS_PATH_MAX + 8];
    struct aml_result r;
    usize n;

    path_join(p, sizeof(p), g_bat[idx].path, bi->method);
    if (aml_evaluate_element(p, 0, 0, bi->str + which, &r) != AML_OK ||
        r.type != AML_T_STRING)
        return -1;
    n = r.bytes_copied < cap - 1 ? r.bytes_copied : cap - 1;
    memcpy(out, r.bytes, n);
    while (n && (out[n - 1] == ' ' || out[n - 1] == 0))
        n--;
    out[n] = 0;
    return (int)n;
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

    /* The unit the firmware reports in is the power unit of _BIX or _BIF:
     * 0 means mW/mWh (energy), 1 means mA/mAh (charge). Read once — it does
     * not change. */
    for (int i = 0; i < g_nbat; i++) {
        struct bat_info bi;
        g_bat[i].energy_units = 1;
        if (bat_info(i, &bi) == 0)
            g_bat[i].energy_units = bi.unit == 0 ? 1 : 0;
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
#define BST_CRITICAL    4u

/* ── charge_behaviour: _BMD and _BMC (ACPI battery maintenance) ────────── */

/* _BMD capability flags (element 1) and status flags (element 0) share these
 * two bits, and _BMC takes them as its argument. */
#define BMD_DISABLE_CHARGING   (1u << 1)
#define BMD_DISCHARGE_ON_AC    (1u << 2)

static const struct {
    const char *name;
    u64 bmc;
} g_charge_modes[] = {
    {"auto", 0},
    {"inhibit-charge", BMD_DISABLE_CHARGING},
    {"force-discharge", BMD_DISCHARGE_ON_AC},
};

static int bat_bmd(int idx, u64 *status, u64 *caps) {
    struct aml_result r;

    if (!acpi_power_battery_has(idx, "_BMD") ||
        eval_pkg(g_bat[idx].path, "_BMD", &r) < 0 || r.elems < 2)
        return -1;
    *status = r.elem_int[0];
    *caps = r.elem_int[1];
    return 0;
}

int acpi_power_battery_has_charge_control(int idx) {
    u64 status, caps;

    if (idx < 0 || idx >= g_nbat || !acpi_power_battery_has(idx, "_BMC") ||
        bat_bmd(idx, &status, &caps) < 0)
        return 0;
    return (caps & (BMD_DISABLE_CHARGING | BMD_DISCHARGE_ON_AC)) != 0;
}

/* Every mode the firmware can do, the one it reports now in brackets --
 * Linux's format for this file. */
static int bat_charge_behaviour(int idx, char *buf, usize cap) {
    u64 status, caps, cur;
    usize n = 0;

    if (bat_bmd(idx, &status, &caps) < 0)
        return -1;
    cur = status & (BMD_DISABLE_CHARGING | BMD_DISCHARGE_ON_AC);
    if (cur & BMD_DISCHARGE_ON_AC)
        cur = BMD_DISCHARGE_ON_AC;
    for (usize k = 0; k < sizeof(g_charge_modes) / sizeof(g_charge_modes[0]); k++) {
        u64 bit = g_charge_modes[k].bmc;
        int w;

        if (bit && !(caps & bit))
            continue;
        w = snprintf(buf + n, cap - n, bit == cur ? "%s[%s]" : "%s%s",
                     n ? " " : "", g_charge_modes[k].name);
        if (w < 0 || (usize)w >= cap - n)
            return -1;
        n += (usize)w;
    }
    if (n + 1 >= cap)
        return -1;
    buf[n++] = '\n';
    buf[n] = 0;
    return (int)n;
}

int acpi_power_battery_set_charge_behaviour(int idx, const char *name) {
    char p[PS_PATH_MAX + 8];
    struct aml_result r;
    u64 status, caps, arg;

    if (!acpi_power_battery_has_charge_control(idx) ||
        bat_bmd(idx, &status, &caps) < 0)
        return -1;
    for (usize k = 0; k < sizeof(g_charge_modes) / sizeof(g_charge_modes[0]); k++) {
        if (strcmp(name, g_charge_modes[k].name))
            continue;
        arg = g_charge_modes[k].bmc;
        if (arg && !(caps & arg))
            return -1;          /* a mode this firmware cannot do */
        path_join(p, sizeof(p), g_bat[idx].path, "_BMC");
        return aml_evaluate(p, &arg, 1, &r) == AML_OK ? 0 : -1;
    }
    return -1;
}

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

    struct bat_info bi;
    if (bat_info(idx, &bi) < 0)
        return -1;

    /* The static ones first: they need no _BST. */
    switch (which) {
    case ACPI_BAT_FULL_DESIGN:
        if (!bi.design || bi.design == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(bi.design * 1000));
    case ACPI_BAT_VOLTAGE_MIN:
        if (!bi.design_mv || bi.design_mv == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(bi.design_mv * 1000));
    case ACPI_BAT_CYCLES:
        if (bi.cycles < 0)
            return -1;
        return snprintf(buf, cap, "%lld\n", (long long)bi.cycles);
    case ACPI_BAT_TECHNOLOGY: {
        /* Linux reads the chemistry from the type string. */
        static const struct { const char *acpi, *name; } chem[] = {
            {"LION", "Li-ion"}, {"LI-ION", "Li-ion"}, {"LIP", "Li-poly"},
            {"LI-POLY", "Li-poly"}, {"NIMH", "NiMH"}, {"NICD", "NiCd"},
            {"LIFE", "LiFe"}, {"LIMN", "LiMn"},
        };
        char t[16];
        if (bat_string(idx, &bi, 2, t, sizeof(t)) < 0)
            return snprintf(buf, cap, "Unknown\n");
        for (usize k = 0; t[k]; k++)
            if (t[k] >= 'a' && t[k] <= 'z')
                t[k] = (char)(t[k] - 32);
        for (usize k = 0; k < sizeof(chem) / sizeof(chem[0]); k++)
            if (!strcmp(t, chem[k].acpi))
                return snprintf(buf, cap, "%s\n", chem[k].name);
        return snprintf(buf, cap, "Unknown\n");
    }
    case ACPI_BAT_MODEL:
    case ACPI_BAT_SERIAL:
    case ACPI_BAT_MANUFACTURER: {
        char t[64];
        u32 k = which == ACPI_BAT_MODEL ? 0 : which == ACPI_BAT_SERIAL ? 1 : 3;
        if (bat_string(idx, &bi, k, t, sizeof(t)) < 0)
            return -1;
        return snprintf(buf, cap, "%s\n", t);
    }
    case ACPI_BAT_ALARM:
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(g_bat[idx].alarm * 1000));
    case ACPI_BAT_CHARGE_BEHAVIOUR:
        return bat_charge_behaviour(idx, buf, cap);
    default:
        break;
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

    u64 full = bi.full;                     /* last full charge capacity */
    if (full == 0 || full == 0xFFFFFFFFULL)
        full = bi.design;                   /* fall back to the design value */

    switch (which) {
    case ACPI_BAT_VOLTAGE: {
        /* _BST's present voltage in mV, or the design voltage when the
         * battery will not say. upower reads this and a laptop's battery
         * indicator is wrong without it. */
        u64 mv = bst.elem_int[3];

        if (!mv || mv == 0xFFFFFFFFULL)
            mv = bi.design_mv;
        if (!mv || mv == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n", (unsigned long long)(mv * 1000));
    }
    case ACPI_BAT_RATE: {
        /* The present rate: mW when the battery reports in energy units, mA
         * when it reports in charge units. Either way sysfs wants micro-. */
        u64 rate = bst.elem_int[1];

        if (rate == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n", (unsigned long long)(rate * 1000));
    }
    case ACPI_BAT_CAPACITY:
        if (!full || remaining == 0xFFFFFFFFULL)
            return -1;
        return snprintf(buf, cap, "%llu\n",
                        (unsigned long long)(remaining * 100 / full));
    case ACPI_BAT_CAPACITY_LEVEL: {
        /* As Linux's acpi battery decides it: critical from _BST, low at or
         * under the alarm the firmware was given, full at the last full
         * charge, normal otherwise. */
        const char *lvl;
        if (state & BST_CRITICAL)
            lvl = "Critical";
        else if (g_bat[idx].alarm && remaining <= g_bat[idx].alarm)
            lvl = "Low";
        else if (full && remaining >= full)
            lvl = "Full";
        else
            lvl = "Normal";
        return snprintf(buf, cap, "%s\n", lvl);
    }
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

int acpi_power_battery_has(int idx, const char *method) {
    char p[PS_PATH_MAX + 8];

    if (idx < 0 || idx >= g_nbat)
        return 0;
    path_join(p, sizeof(p), g_bat[idx].path, method);
    return aml_exists(p);
}

/* _BTP: the firmware notifies when the remaining capacity crosses this. */
int acpi_power_battery_set_alarm(int idx, u64 micro) {
    char p[PS_PATH_MAX + 8];
    struct aml_result r;
    u64 milli = micro / 1000;

    if (idx < 0 || idx >= g_nbat || !acpi_power_battery_has(idx, "_BTP"))
        return -1;
    path_join(p, sizeof(p), g_bat[idx].path, "_BTP");
    if (aml_evaluate(p, &milli, 1, &r) != AML_OK)
        return -1;
    g_bat[idx].alarm = milli;
    return 0;
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
