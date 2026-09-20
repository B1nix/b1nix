/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Qualcomm restart reasons: reboot(LINUX_REBOOT_CMD_RESTART2, "bootloader").
 *
 * On a Snapdragon the bootloader (ABL) decides where the next boot goes from
 * two places the OS fills in before it resets:
 *
 *   - the PMIC's PON block, register SOFT_RB_SPARE, which survives any kind
 *     of reset because it lives in the PMIC, and
 *   - the restart_reason word in the SoC's IMEM, which survives only a warm
 *     reset (the PMIC's PS_HOLD reset type is set to "warm" for that).
 *
 * Both are the values Android's msm-poweroff and qpnp-power-on drivers write.
 * The PMIC is reached over SPMI through the PMIC arbiter (spmi-pmic-arb,
 * version 5 on SM8150): one write channel per peripheral, owned by an
 * execution environment; the APPS EE is `qcom,ee` in the device tree.
 * Everything here is polled and runs with interrupts off from SYS_REBOOT.
 */
#include <b1nix/types.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <string.h>

/* PMIC arbiter, `core` block. */
#define PMIC_ARB_VERSION          0x0000
#define PMIC_ARB_VERSION_V5_MIN   0x50000000u
#define PMIC_ARB_VERSION_V7_MIN   0x70000000u
#define PMIC_ARB_V5_APID_MAP(n)   (0x900 + 4 * (n))   /* PPID of APID n */
#define PMIC_ARB_V5_MAX_APIDS     512
#define PMIC_ARB_PPID_MASK        0xfffu
#define PMIC_ARB_APID_IRQ_OWNER   (1u << 24)
/* `cnfg` block: which EE may write through APID n. */
#define PMIC_ARB_OWNERSHIP(n)     (0x700 + 4 * (n))
#define PMIC_ARB_OWNER_MASK       0x7u

/* One channel (a `chnls` write slot or an `obsrvr` read slot). */
#define PMIC_ARB_CMD              0x00
#define PMIC_ARB_STATUS           0x08
#define PMIC_ARB_WDATA0           0x10
#define PMIC_ARB_RDATA0           0x18
#define PMIC_ARB_STATUS_DONE      (1u << 0)
#define PMIC_ARB_STATUS_FAILURE   (1u << 1)
#define PMIC_ARB_STATUS_DENIED    (1u << 2)
#define PMIC_ARB_STATUS_DROPPED   (1u << 3)
#define PMIC_ARB_OP_EXT_WRITEL    0
#define PMIC_ARB_OP_EXT_READL     1
#define PMIC_ARB_V5_WR_SLOT       0x10000   /* chnls + apid * this */
#define PMIC_ARB_V5_RD_EE_STRIDE  0x10000   /* obsrvr + ee * this + apid * 0x80 */
#define PMIC_ARB_V5_RD_APID_STRIDE 0x80
#define PMIC_ARB_TIMEOUT_US       100

/* PON (power-on) block, generation 2 (pm8998/pm8150 family). */
#define PON_REASON1               0x08
#define PON_WARM_RESET_REASON1    0x0a
#define PON_PS_HOLD_RST_CTL       0x5a
#define PON_PS_HOLD_RST_CTL2      0x5b
#define PON_SOFT_RB_SPARE         0x8f
#define PON_RESET_TYPE_MASK       0x0fu
#define PON_RESET_TYPE_WARM       0x01u
#define PON_RESET_EN              (1u << 7)
/* SOFT_RB_SPARE holds the reason in bits 7:1 on this PON generation (bits
 * 7:2 on the first one, pm8916/pm8941; Linux's GEN2_REASON_SHIFT). */
#define PON_RESTART_REASON_SHIFT  1

/* When the tree carries no PON node (the phone's PMIC overlay is not part
 * of the extracted SoC tree) the primary PMIC's PON is at the address every
 * Qualcomm PMIC puts it: peripheral 0x800 on slave 0. The other PMICs on the
 * bus (pm8150b, pm8150l on SM8150) each have their own PON at the same
 * peripheral on their primary slave id, the even ones, and each decides on
 * its own what PS_HOLD going low does to its rails: a warm reset needs all
 * of them to agree, as Android's qpnp_pon_system_pwr_off configures them. */
#define PON_DEFAULT_SID           0
#define PON_DEFAULT_BASE          0x800
#define PON_MAX_SID               15

/* What Android writes for each reboot argument: the PON reason (enum
 * pon_restart_reason) and the IMEM magic (msm-poweroff.c). */
static const struct {
	const char *cmd;
	u8 pon;
	u32 imem;
} reasons[] = {
	{ "bootloader",                 2, 0x77665500u },
	{ "recovery",                   1, 0x77665502u },
	{ "rtc",                        3, 0x77665503u },
	{ "dm-verity device corrupted", 4, 0x77665508u },
	{ "dm-verity enforcing",        5, 0x77665509u },
	{ "keys clear",                 6, 0x7766550au },
};

/* Qualcomm SIP calls (SMCCC, 64-bit): service 9 is power, command 1 halts
 * the PMIC arbiter and command 2 deasserts PS_HOLD. Both take one zero
 * argument (Android's msm-poweroff). */
#define QCOM_SCM_PWR_DISABLE_PMIC_ARBITER 0xc2000901u
#define QCOM_SCM_PWR_DEASSERT_PS_HOLD     0xc2000902u

static u64 qcom_scm_call(u32 fn)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = 1;   /* argument descriptor: one value */
	register u64 x2 __asm__("x2") = 0;   /* the value, 0 */
	register u64 x3 __asm__("x3") = 0;
	register u64 x4 __asm__("x4") = 0;
	register u64 x5 __asm__("x5") = 0;
	register u64 x6 __asm__("x6") = 0;

	__asm__ volatile("smc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
	                 :
	                 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
	                   "x15", "x16", "x17", "memory");
	return x0;
}

static inline u32 rd32(volatile void *base, u32 off)
{
	return *(volatile u32 *)((volatile u8 *)base + off);
}

static inline void wr32(volatile void *base, u32 off, u32 v)
{
	*(volatile u32 *)((volatile u8 *)base + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

struct spmi_arb {
	volatile void *core;
	volatile void *cnfg;
	u64 chnls_phys;
	u64 obsrvr_phys;
	u32 ee;
};

/* The write APID for a PPID: the arbiter maps one PPID to several APIDs so
 * that more than one EE can write it; ours is the one whose owner is our EE
 * (or the first one at all, which then reports DENIED if it is not ours). */
static void step(const char *what, u64 v)
{
	console_write("reboot: spmi: ");
	console_write(what);
	console_write(" 0x");
	console_write_hex64(v);
	console_write("\n");
}

static int spmi_find_apid(struct spmi_arb *a, u32 ppid, u32 *apid_out, u32 *owner_out)
{
	int found = -1;
	u32 found_owner = 0;

	for (u32 i = 0; i < PMIC_ARB_V5_MAX_APIDS; i++) {
		u32 map = rd32(a->core, PMIC_ARB_V5_APID_MAP(i));

		if (!map || ((map >> 8) & PMIC_ARB_PPID_MASK) != ppid)
			continue;
		u32 owner = rd32(a->cnfg, PMIC_ARB_OWNERSHIP(i)) & PMIC_ARB_OWNER_MASK;

		if (found < 0 || owner == a->ee) {
			found = (int)i;
			found_owner = owner;
		}
		if (owner == a->ee)
			break;
	}
	if (found < 0)
		return -1;
	*apid_out = (u32)found;
	*owner_out = found_owner;
	return 0;
}

static int spmi_wait_done(volatile void *slot)
{
	for (u32 t = 0; t < PMIC_ARB_TIMEOUT_US; t++) {
		u32 st = rd32(slot, PMIC_ARB_STATUS);

		if (st & PMIC_ARB_STATUS_DONE) {
			if (st & (PMIC_ARB_STATUS_DROPPED | PMIC_ARB_STATUS_FAILURE |
			          PMIC_ARB_STATUS_DENIED))
				return -(int)st;
			return 0;
		}
		arch_udelay(1);
	}
	return -1;
}

static u32 spmi_cmd(u32 opc, u32 addr, u32 bytes)
{
	return (opc << 27) | ((addr & 0xff) << 4) | ((bytes - 1) & 0x7);
}

static int spmi_read8(struct spmi_arb *a, u32 sid, u32 addr, u8 *out)
{
	u32 apid, owner, ppid = (sid << 8) | (addr >> 8);

	if (spmi_find_apid(a, ppid, &apid, &owner))
		return -1;
	/* Reads go through this EE's own observer slot, whoever owns the APID. */
	volatile void *slot = vmm_map_mmio(a->obsrvr_phys + (u64)a->ee * PMIC_ARB_V5_RD_EE_STRIDE +
	                                   (u64)apid * PMIC_ARB_V5_RD_APID_STRIDE,
	                                   0x1000, VMM_WRITABLE);
	if (!slot)
		return -1;
	wr32(slot, PMIC_ARB_CMD, spmi_cmd(PMIC_ARB_OP_EXT_READL, addr, 1));
	int rc = spmi_wait_done(slot);
	if (rc == 0)
		*out = (u8)rd32(slot, PMIC_ARB_RDATA0);
	return rc;
}

static int spmi_write8(struct spmi_arb *a, u32 sid, u32 addr, u8 val)
{
	u32 apid, owner, ppid = (sid << 8) | (addr >> 8);

	if (spmi_find_apid(a, ppid, &apid, &owner))
		return -1;
	/* A write channel belongs to one EE. Touching another EE's slot does
	 * not come back as DENIED on this phone: the access never returns and
	 * the CPU is gone (seen with the PON's neighbour, APID 0xe owned by EE
	 * 4). Linux refuses the same case with EPERM. */
	if (owner != a->ee) {
		console_write("reboot: spmi: ppid 0x");
		console_write_hex32(ppid);
		console_write(" write channel belongs to EE ");
		console_write_dec(owner);
		console_write(", not ours\n");
		return -1;
	}
	volatile void *slot = vmm_map_mmio(a->chnls_phys + (u64)apid * PMIC_ARB_V5_WR_SLOT,
	                                   0x1000, VMM_WRITABLE);
	if (!slot)
		return -1;
	wr32(slot, PMIC_ARB_WDATA0, val);
	wr32(slot, PMIC_ARB_CMD, spmi_cmd(PMIC_ARB_OP_EXT_WRITEL, addr, 1));
	return spmi_wait_done(slot);
}

static int spmi_update8(struct spmi_arb *a, u32 sid, u32 addr, u8 mask, u8 val)
{
	u8 cur;

	if (spmi_read8(a, sid, addr, &cur))
		return -1;
	return spmi_write8(a, sid, addr, (u8)((cur & ~mask) | (val & mask)));
}

static int spmi_arb_open(struct spmi_arb *a)
{
	u64 core, chnls, obsrvr, cnfg;

	if (!fdt_qcom_spmi_arb(&core, &chnls, &obsrvr, &cnfg, &a->ee))
		return -1;
	a->core = vmm_map_mmio(core, 0x2000, VMM_WRITABLE);
	a->cnfg = vmm_map_mmio(cnfg, 0x1000 + PMIC_ARB_OWNERSHIP(PMIC_ARB_V5_MAX_APIDS), VMM_WRITABLE);
	a->chnls_phys = chnls;
	a->obsrvr_phys = obsrvr;
	if (!a->core || !a->cnfg)
		return -1;

	u32 ver = rd32(a->core, PMIC_ARB_VERSION);
	step("arbiter version", ver);
	step("ee", a->ee);
	if (ver < PMIC_ARB_VERSION_V5_MIN || ver >= PMIC_ARB_VERSION_V7_MIN) {
		console_write("reboot: spmi arbiter version 0x");
		console_write_hex32(ver);
		console_write(" is not v5, restart reason not set\n");
		return -1;
	}
	return 0;
}

static void report(const char *what, int rc)
{
	console_write("reboot: ");
	console_write(what);
	if (rc == 0) {
		console_write(" ok\n");
		return;
	}
	console_write(" failed (");
	if (rc < -1) {
		console_write("status 0x");
		console_write_hex32((u32)-rc);
	} else {
		console_write("no channel or timeout");
	}
	console_write(")\n");
}

/* Called from SYS_REBOOT with the RESTART2 argument, interrupts off, just
 * before the PSCI reset. Silent on a machine without a PMIC arbiter. */
void aarch64_qcom_set_restart_reason(const char *cmd)
{
	struct spmi_arb arb;
	u32 sid = PON_DEFAULT_SID, pon = PON_DEFAULT_BASE;
	const char *match = 0;
	u8 pon_reason = 0;
	u32 imem_magic = 0x77665501u;   /* Android's "normal restart" */

	if (!cmd)
		return;
	for (u32 i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
		if (strcmp(cmd, reasons[i].cmd) == 0) {
			match = reasons[i].cmd;
			pon_reason = reasons[i].pon;
			imem_magic = reasons[i].imem;
			break;
		}
	}
	if (!match) {
		console_write("reboot: unknown restart reason, plain restart\n");
		return;
	}

	/* IMEM first: it needs no bus, and it is what ABL reads on SoCs that
	 * do not consult the PMIC. */
	u64 imem = fdt_qcom_restart_reason_addr();
	if (imem) {
		volatile void *p = vmm_map_mmio(imem & ~0xfffULL, 0x1000, VMM_WRITABLE);

		if (p) {
			wr32(p, (u32)(imem & 0xfff), imem_magic);
			console_write("reboot: imem restart_reason = 0x");
			console_write_hex32(imem_magic);
			console_write("\n");
		}
	}

	if (spmi_arb_open(&arb))
		return;
	fdt_qcom_pon(&sid, &pon);
	step("pon sid", sid);
	step("pon base", pon);
	{
		u32 apid, owner;

		if (spmi_find_apid(&arb, (sid << 8) | (pon >> 8), &apid, &owner) == 0) {
			step("pon apid", apid);
			step("pon apid owner", owner);
		} else {
			console_write("reboot: spmi: no channel maps the PON\n");
		}
	}

	/* A warm reset so that IMEM keeps the word: PS_HOLD's reset type is
	 * changed with the reset disabled, as the PMIC's programming guide (and
	 * qpnp-power-on) does it, and a settle delay after the disable. On
	 * every PMIC whose PON answers, not just the one the reason goes to. */
	int rc = 0;

	for (u32 s = 0; s <= PON_MAX_SID; s += 2) {
		u8 probe;
		u32 warm_sid = s == 0 ? sid : s;

		if (s != 0 && warm_sid == sid)
			continue;
		if (spmi_read8(&arb, warm_sid, pon + PON_PS_HOLD_RST_CTL, &probe))
			continue;
		int r = spmi_update8(&arb, warm_sid, pon + PON_PS_HOLD_RST_CTL2, PON_RESET_EN, 0);
		arch_udelay(300);
		if (r == 0)
			r = spmi_update8(&arb, warm_sid, pon + PON_PS_HOLD_RST_CTL,
			                 PON_RESET_TYPE_MASK, PON_RESET_TYPE_WARM);
		if (r == 0)
			r = spmi_update8(&arb, warm_sid, pon + PON_PS_HOLD_RST_CTL2,
			                 PON_RESET_EN, PON_RESET_EN);
		console_write("reboot: pmic sid ");
		console_write_dec(warm_sid);
		console_write(" ps_hold reset type was 0x");
		console_write_hex32(probe);
		console_write("\n");
		report("pmic warm reset", r);
		if (r)
			rc = r;
	}

	rc = spmi_update8(&arb, sid, pon + PON_SOFT_RB_SPARE,
	                  (u8)(0xffu << PON_RESTART_REASON_SHIFT),
	                  (u8)(pon_reason << PON_RESTART_REASON_SHIFT));
	if (rc == 0) {
		u8 back = 0;

		rc = spmi_read8(&arb, sid, pon + PON_SOFT_RB_SPARE, &back);
		if (rc == 0 && (back >> PON_RESTART_REASON_SHIFT) != pon_reason)
			rc = -1;
	}
	report("pmic pon restart reason", rc);

	/* Reset the way Android does it, by dropping PS_HOLD ourselves, so that
	 * the PMIC performs the reset type just configured. A PSCI SYSTEM_RESET
	 * goes through the secure firmware, which may pick its own. The log so
	 * far goes onto the flash first: the periodic mirror will not run again,
	 * and the previous boot's /proc/last_kmsg is how these lines are read
	 * on a phone without a serial port. */
	extern void ufs_log_reboot_flush(void);

	if (rc == 0) {
		u64 pshold = fdt_qcom_pshold_addr();
		volatile void *p = pshold ? vmm_map_mmio(pshold & ~0xfffULL, 0x1000, VMM_WRITABLE) : 0;

		/* The secure firmware's own way first, which Android uses on this
		 * SoC generation: halt the PMIC arbiter, then deassert PS_HOLD. A
		 * plain store to the PS_HOLD register resets the SoC too, but on
		 * this phone it came out as a cold reset whatever the PMIC was
		 * told, and the reason does not survive that. */
		console_write("reboot: SCM deassert PS_HOLD\n");
		ufs_log_reboot_flush();
		u64 r1 = qcom_scm_call(QCOM_SCM_PWR_DISABLE_PMIC_ARBITER);
		u64 r2 = qcom_scm_call(QCOM_SCM_PWR_DEASSERT_PS_HOLD);
		arch_udelay(1000000);
		console_write("reboot: SCM did not reset the SoC (0x");
		console_write_hex64(r1);
		console_write(", 0x");
		console_write_hex64(r2);
		console_write(")\n");
		if (p) {
			console_write("reboot: dropping PS_HOLD\n");
			ufs_log_reboot_flush();
			wr32(p, (u32)(pshold & 0xfff), 0);
			arch_udelay(1000000);
			console_write("reboot: PS_HOLD write did not reset the SoC, PSCI next\n");
		}
	}
	ufs_log_reboot_flush();
}

/* At boot: what the PMIC and IMEM say about the last reset, and what they
 * hold now. PON_REASON1 names the power-on trigger, WARM_RESET_REASON1 is
 * non-zero after a warm reset, SOFT_RB_SPARE and the IMEM word are the
 * restart reason the previous run left (the bootloader clears them once it
 * has acted on them). Reads only. */
void aarch64_qcom_report_reset_reason(void)
{
	struct spmi_arb arb;
	u32 sid = PON_DEFAULT_SID, pon = PON_DEFAULT_BASE;
	u64 imem = fdt_qcom_restart_reason_addr();

	if (imem) {
		volatile void *p = vmm_map_mmio(imem & ~0xfffULL, 0x1000, VMM_WRITABLE);

		if (p) {
			console_write("pon: imem restart_reason 0x");
			console_write_hex32(rd32(p, (u32)(imem & 0xfff)));
			console_write("\n");
		}
	}
	if (spmi_arb_open(&arb))
		return;
	fdt_qcom_pon(&sid, &pon);
	static const struct { const char *name; u32 off; } regs[] = {
		{ "reason1", PON_REASON1 },
		{ "warm_reset_reason1", PON_WARM_RESET_REASON1 },
		{ "ps_hold_rst_ctl", PON_PS_HOLD_RST_CTL },
		{ "ps_hold_rst_ctl2", PON_PS_HOLD_RST_CTL2 },
		{ "soft_rb_spare", PON_SOFT_RB_SPARE },
	};
	for (u32 s = 0; s <= PON_MAX_SID; s += 2) {
		u8 probe;
		u32 this_sid = s == 0 ? sid : s;

		if (s != 0 && this_sid == sid)
			continue;
		if (spmi_read8(&arb, this_sid, pon + PON_REASON1, &probe))
			continue;
		console_write("pon: sid ");
		console_write_dec(this_sid);
		for (u32 i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
			u8 v = 0;

			console_write(" ");
			console_write(regs[i].name);
			console_write("=");
			if (spmi_read8(&arb, this_sid, pon + regs[i].off, &v) == 0)
				console_write_hex32(v);
			else
				console_write("err");
		}
		console_write("\n");
	}
}
