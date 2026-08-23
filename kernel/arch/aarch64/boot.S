.section .text.boot
.global _start
.type _start, %function

.extern kernel_main
.extern __bss_start
.extern __bss_end

	// arm64 Linux image header (Documentation/arm64/booting.rst). Without it
	// QEMU treats the image as a bare-metal blob: it loads it, zeroes the
	// registers and never builds a device tree — so x0 is 0 and -append is
	// silently dropped. With the header QEMU boots it as a Linux kernel:
	// loads at RAM base + text_offset (0x40000000 + 0x80000 = our link
	// address), materialises the DTB with the -append bootargs in
	// /chosen, and passes its address in x0. Requires the raw Image
	// (build/aarch64/Image), not the ELF.
_start:
	b	_start_kernel		// code0: executable branch
	.long	0			// code1
	.quad	0x80000			// text_offset from RAM base
	.quad	_kernel_size		// image_size (incl. bss)
	.quad	0			// flags: LE, 4K pages, fixed load offset
	.quad	0			// res2
	.quad	0			// res3
	.quad	0			// res4
	.ascii	"ARM\x64"		// magic
	.long	0			// res5 (PE header offset, unused)

_start_kernel:
	// Save DTB address from x0 to x19 FIRST (before any register clobber)
	mov x19, x0

	// Early raw UART strobe directly in assembly (PL011 & Mini UART)
	ldr x1, =0xfe201000
	mov w2, #'B'
	str w2, [x1]
	mov w2, #'1'
	str w2, [x1]

	ldr x1, =0xfe215040
	mov w2, #'B'
	str w2, [x1]
	mov w2, #'1'
	str w2, [x1]

#ifdef B1NIX_UART_AUTOBAUD
	// Transmit 'U' forever on both ports (see UART_AUTOBAUD in the Makefile).
	// No flag polling and a crude delay instead: the point is a continuous
	// stream whose bit pattern is unambiguous at any sample rate, not
	// throughput.
.autobaud_loop:
	ldr x1, =0xfe201000
	mov w2, #'U'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]
	mov x3, #0x2000
.autobaud_delay:
	subs x3, x3, #1
	b.ne .autobaud_delay
	b .autobaud_loop
#endif

	// Down to EL1, whichever level this CPU was handed to us at.
	bl .drop_to_el1

	// Marker 'E' = EL1 drop completed
	ldr x1, =0xfe201000
	mov w2, #'E'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]

	// ── One boot CPU ────────────────────────────────────────────────────────
	mrs x0, mpidr_el1
	and x20, x0, #0xff
	cbnz x20, .park_secondary

	// Marker 'P' = Primary CPU confirmed
	ldr x1, =0xfe201000
	mov w2, #'P'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]

	adr x4, _start			// where the loader actually put us
	ldr x5, =__kernel_start		// where the linker put us
	cmp x4, x5
	b.eq .relocated
	ldr x6, =__bss_start
	sub x6, x6, x5			// bytes of initialised image
	mov x7, xzr
.copy_image:
	ldr x8, [x4, x7]
	str x8, [x5, x7]
	add x7, x7, #8
	cmp x7, x6
	b.lo .copy_image
	dsb sy
	ic ialluis			// the copy was data writes; this is now code
	dsb sy
	isb
	ldr x8, =.relocated
	br x8
.relocated:

	// Marker 'R' = Relocation step done
	ldr x1, =0xfe201000
	mov w2, #'R'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]


	// Disable alignment check in SCTLR_EL1
	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 1)
	msr sctlr_el1, x0
	isb

	// CPACR_EL1.FPEN = 0b11: grant EL0 and EL1 full access to FP/SIMD/NEON.
	// The kernel itself builds -mgeneral-regs-only and never traps here, but
	// userspace (musl, ld.so, any NEON codegen) does — left at its reset
	// default (trap-all on most implementations) this faults with ESR EC=0x07
	// on the very first FP/SIMD instruction musl's _dlstart executes.
	mov x0, #(0b11 << 20)
	msr cpacr_el1, x0
	isb

	// Setup MAIR_EL1: Attr0 = 0x04 (Device-nGnRE), Attr1 = 0xFF (Normal WB),
	// Attr2 = 0x44 (Normal Non-cacheable inner+outer). Attr2 is what
	// ioremap_wc() asks for: ARM has no "write-combining" memory type by that
	// name, and Normal-NC is its equivalent — writes may be merged and
	// reordered, which is the whole point for a framebuffer or a GTT, while
	// Device memory (Attr0) forbids exactly that.
	ldr x0, =0x000000000044ff04
	msr mair_el1, x0

	// Setup TCR_EL1: T0SZ=16, TG0=4KB, Inner/Outer WBWA, IS. The constant
	// carries IPS=2 (40-bit output) as its default; the real width is not a
	// constant, so take it from the processor: ID_AA64MMFR0_EL1.PARange uses
	// exactly the IPS encoding. Hardcoding 40 bits asks a CPU that implements
	// fewer for an output size it does not have (the field is then CONSTRAINED
	// UNPREDICTABLE), and caps a wider one at 1 TiB for no reason. Clamp to 5
	// (48-bit): 6 means 52-bit, which needs FEAT_LPA and a different
	// descriptor format than the tables here use.
	ldr x0, =0x2b5103510
	mrs x1, id_aa64mmfr0_el1
	and x1, x1, #0xf
	mov x2, #5
	cmp x1, x2
	csel x1, x1, x2, lo
	bic x0, x0, #(7 << 32)
	orr x0, x0, x1, lsl #32
	msr tcr_el1, x0

	// Build the boot identity map. It used to be four hand-written 1 GiB
	// blocks — Device for 0-1 GiB and Normal for 1-4 GiB — which is QEMU
	// virt's memory map and no other board's: a Raspberry Pi 4 has its RAM at
	// 0 (this image included) and its peripherals at 0xfc000000, so both of
	// those blocks had the wrong memory type and the kernel could neither
	// execute from RAM nor reach its UART.
	//
	// So fill all 512 entries here instead, covering 0-512 GiB as
	// Device-nGnRE, and mark Normal the two blocks that must be: the one
	// holding this image (found from the PC, so it needs no constant) and the
	// one holding the device tree. That is everything the code between here
	// and the real map touches — it walks that tree, and the tree names the
	// RAM banks the map is then rebuilt from (aarch64_boot_map_rebuild,
	// kernel/arch/aarch64/paging.c).
	ldr x0, =boot_l1
	mov x1, xzr			// block index
	mov x2, #0x401			// valid block | AF, AttrIndx 0 (Device)
.fill_l1:
	lsl x3, x1, #30			// block address = index * 1 GiB
	orr x3, x3, x2
	str x3, [x0, x1, lsl #3]
	add x1, x1, #1
	cmp x1, #512
	b.lo .fill_l1

	mov x2, #0x405			// valid block | AF | AttrIndx 1 (Normal WB)
	adr x3, _start			// this image's physical address
	lsr x3, x3, #30			// ... as a 1 GiB block index
	lsl x4, x3, #30
	orr x4, x4, x2
	str x4, [x0, x3, lsl #3]

	lsr x3, x19, #30		// the same for the device tree in x0 at entry
	lsl x4, x3, #30
	orr x4, x4, x2
	str x4, [x0, x3, lsl #3]
	dsb sy

	// Load TTBR0_EL1 & TTBR1_EL1 with boot_l0
	ldr x0, =boot_l0
	msr ttbr0_el1, x0
	msr ttbr1_el1, x0
	isb

	// Invalidate TLB
	tlbi vmalle1is
	dsb ish
	isb

	// Enable MMU only (M=1, C=0, I=0, WXN=0) — clean slate
	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 1)   // A = 0 (alignment check off)
	bic x0, x0, #(1 << 2)   // C = 0
	bic x0, x0, #(1 << 12)  // I = 0
	bic x0, x0, #(1 << 19)  // WXN = 0 (writable ≠ XN)
	bic x0, x0, #(1 << 25)  // EE = 0 (little-endian)
	orr x0, x0, #(1 << 0)   // M = 1
	msr sctlr_el1, x0
	isb

	// Marker 'M' = MMU enabled!
	ldr x1, =0xfe201000
	mov w2, #'M'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]

	// Set up stack
	ldr x0, =stack_top
	mov sp, x0

	// Clear BSS
	ldr x0, =__bss_start
	ldr x1, =__bss_end
.clear_bss:
	cmp x0, x1
	b.hs .bss_cleared
	str xzr, [x0], #8
	b .clear_bss
.bss_cleared:

	// Marker 'S' = Stack & BSS ready!
	ldr x1, =0xfe201000
	mov w2, #'S'
	str w2, [x1]
	ldr x1, =0xfe215040
	str w2, [x1]

	// Restore DTB address to x0, arg1 to x1
	mov x0, x19
	mov x1, #0

	// Marker 'K' = Jumping to kernel_main!
	ldr x2, =0xfe201000
	mov w3, #'K'
	str w3, [x2]
	ldr x2, =0xfe215040
	str w3, [x2]
	mov w3, #'\r'
	str w3, [x2]
	mov w3, #'\n'
	str w3, [x2]
	
	// Jump to kernel_main
	bl kernel_main


.hang:
	wfe
	b .hang

// ── Down to EL1 ─────────────────────────────────────────────────────────────
//
// Returns to its caller (x30) running at EL1, having dropped through whatever
// levels this CPU implements. QEMU virt hands the kernel EL1 and this code used
// to assume it; QEMU's raspi4b hands it EL3, where SCTLR_EL1 and TTBR0_EL1 are
// writable and ignored — the MMU below would appear to come up while every
// address stayed physical. Secondaries need it just as much as the boot CPU: a
// spin-table release enters at whatever level the parked CPU was left at.
//
// Uses x0 only, and no stack — the boot CPU calls this before it has one.
.drop_to_el1:
	mrs x0, CurrentEL
	lsr x0, x0, #2
	cmp x0, #3
	b.ne .maybe_el2

	mov x0, #0x531			// SCR_EL3: NS | RES1 | HCE | RW (lower ELs AArch64)
	msr scr_el3, x0
	msr cptr_el3, xzr		// no FP/SIMD trap on the way down
	mov x0, #0x3c9			// SPSR_EL3: DAIF masked, return to EL2h
	msr spsr_el3, x0
	adr x0, .drop_from_el2
	msr elr_el3, x0
	eret

.maybe_el2:
	cmp x0, #2
	b.ne .already_el1
.drop_from_el2:
	// Marker '2' = In EL2 drop handler
	ldr x1, =0xfe201000
	mov w2, #'2'
	str w2, [x1]

	// EL1's execution state has to be set explicitly: HCR_EL2.RW = 1 (AArch64)
	movz x0, #0x8000, lsl #16
	msr hcr_el2, x0
	mov x0, #0x33ff			// CPTR_EL2: no FP/SIMD/SVE trap below
	msr cptr_el2, x0
	msr hstr_el2, xzr
	mrs x0, cnthctl_el2
	orr x0, x0, #3			// EL1 may use the physical counter and timer
	msr cnthctl_el2, x0
	msr cntvoff_el2, xzr		// virtual counter == physical counter

	// Preserve RES1 bits in SCTLR_EL1, turn off MMU and caches
	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 0)		// M = 0 (MMU off)
	bic x0, x0, #(1 << 1)		// A = 0 (Alignment check off)
	bic x0, x0, #(1 << 2)		// C = 0 (Data cache off)
	bic x0, x0, #(1 << 12)		// I = 0 (Instruction cache off)
	msr sctlr_el1, x0

	mov x0, #0x3c5			// SPSR_EL2: DAIF masked, return to EL1h
	msr spsr_el2, x0
	msr elr_el2, x30		// ... at the caller's return address
	eret

.already_el1:
	// Marker '1' = Already in EL1
	ldr x1, =0xfe201000
	mov w2, #'1'
	str w2, [x1]
	ret


// A secondary that arrived at _start_kernel because nothing parked it. It
// waits at EL1 with the MMU off, which is the state _ap_start expects, and
// takes no stack — there is exactly one boot stack and the boot CPU owns it.
.park_secondary:
	ldr x0, =g_spin_release
1:
	wfe
	ldr x1, [x0, x20, lsl #3]
	cbz x1, 1b
	br x1

// ── Secondary CPU entry ─────────────────────────────────────────────────────
//
// PSCI CPU_ON hands the woken CPU this address (a PHYSICAL one: the MMU is
// off and caches are cold) with x0 = the context id the caller passed, which
// this kernel uses as the CPU index. Everything here is the BSP's prologue
// minus the parts that are the system's rather than the CPU's — no BSS clear,
// no device tree, and the page tables are the live kernel ones rather than
// boot_l0, because by now the heap and every MMIO window live outside the
// boot identity map.
//
// The kernel image is loaded where it is linked, so reading g_ap_ttbr0 and
// g_ap_sp with the MMU off works: their virtual addresses are their physical
// ones.
.global _ap_start
.type _ap_start, %function
_ap_start:
	// A spin-table release enters at whatever exception level the firmware
	// parked this CPU at, which on a Raspberry Pi is not EL1.
	bl .drop_to_el1

	// This CPU's index, from the table the BSP filled in before releasing it.
	// PSCI's CPU_ON can carry it in a context id and a spin-table release
	// cannot — the firmware's parked loop enters with x0 = 0 — so both paths
	// use the affinity the CPU reports about itself.
	mrs x0, mpidr_el1
	and x0, x0, #0xff
	ldr x1, =g_aff0_to_cpu
	ldrb w20, [x1, x0]

	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 1)		// A = 0: unaligned accesses allowed
	msr sctlr_el1, x0
	isb

	mov x0, #(0b11 << 20)		// CPACR_EL1.FPEN: EL0/EL1 may use FP/SIMD
	msr cpacr_el1, x0
	isb

	ldr x0, =0x000000000044ff04	// MAIR: Device-nGnRE / Normal WB / Normal-NC
	msr mair_el1, x0

	ldr x0, =0x2b5103510		// TCR, with IPS taken from this CPU
	mrs x1, id_aa64mmfr0_el1
	and x1, x1, #0xf
	mov x2, #5
	cmp x1, x2
	csel x1, x1, x2, lo
	bic x0, x0, #(7 << 32)
	orr x0, x0, x1, lsl #32
	msr tcr_el1, x0

	ldr x0, =g_ap_ttbr0		// the live kernel L0, published by the BSP
	ldr x0, [x0]
	msr ttbr0_el1, x0
	msr ttbr1_el1, x0
	isb

	tlbi vmalle1
	dsb nsh
	isb

	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 2)		// C = 0
	bic x0, x0, #(1 << 12)		// I = 0
	bic x0, x0, #(1 << 19)		// WXN = 0
	bic x0, x0, #(1 << 25)		// EE = 0
	orr x0, x0, #(1 << 0)		// M = 1
	msr sctlr_el1, x0
	isb

	// Exception vectors: shared with the BSP, but VBAR_EL1 is per-CPU.
	ldr x0, =vector_table_el1
	msr vbar_el1, x0
	isb

	// This CPU's stack, from the table the BSP filled in before the call.
	ldr x0, =g_ap_sp
	ldr x1, [x0, x20, lsl #3]
	mov sp, x1

	mov x0, x20
	bl aarch64_ap_main
	b .hang

.section .bss
.align 12
stack_bottom:
	.skip 65536 // matches x86_64 boot.S / KERNEL_STACK_SIZE — 16KB was not
	            // enough for the ELF+interpreter-loading call chain userspace_init
	            // runs on this stack before any task switch (wild-jump crash).
.global stack_top
stack_top:

.section .data
// One release slot per affinity-0 value, written by smp_boot_aps
// (kernel/arch/aarch64/smp.c) when it starts a spin-table CPU. Indexed by the
// affinity the parked CPU reads out of its own MPIDR, so it is as wide as that
// field. In .data rather than .bss: a parked CPU is already polling it while
// the boot CPU clears the BSS.
.align 3
.global g_spin_release
g_spin_release:
	.fill 256, 8, 0

.align 12
.global boot_l0
boot_l0:
	.quad boot_l1 + 3
	.fill 511, 8, 0

// One 1 GiB block per entry, covering 0-512 GiB. Written by the loop in
// _start_kernel above and rewritten from the device tree's RAM banks by
// aarch64_boot_map_rebuild() once the tree has been read, so nothing here
// depends on a particular board's memory map.
.global boot_l1
boot_l1:
	.fill 512, 8, 0
