/*
 * mido GPU bring-up, stage 1 (shim test only): power + clocks + register read.
 *
 * Adreno 506 (mido.dtb qcom,kgsl-3d0@1c00000: chipid 0x05000600, regs 0x01c00000
 * 0x40000, irq SPI 33; SMMU qcom,smmu-v2 @0x01c40000 tz-device-id GPU, skip-init).
 * Power domains: gdsc_oxili_cx 0x0185904c, gdsc_oxili_gx 0x0185901c (force-enable
 * root clock). The GPU runs from VDD_CX (msm8953-gfx-ldo bypassed at the upper
 * corners), so 19.2 MHz at the boot CX corner needs no regulator change.
 * GCC gfx block 0x01859000 (Linux clock-gcc-8953 family layout): GFX3D RCG CMD
 * +0x000 / CFG +0x004, branch CBCRs in 0x59020..0x59044 (gfx3d, ahb, bimc_gfx,
 * timer, aon ...). The block is dumped first so the layout can be checked.
 * GDSCR: bit0 SW_COLLAPSE (1 = off), bit31 PWR_ON. CBCR: bit0 CLK_ENABLE, bit31 CLK_OFF.
 * Reference for the overall approach: ios-sailfish's Adreno 530 bring-up (study only).
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static void delay_us(unsigned us) { uint64_t e = now() + 19200ULL * us / 1000 + 1; while (now() < e) ; }

#define GCC_GFX       0x01859000UL
#define GFX3D_CMD     (GCC_GFX + 0x000)
#define GFX3D_CFG     (GCC_GFX + 0x004)
#define GX_GDSCR      0x0185901cUL
#define CX_GDSCR      0x0185904cUL
#define GPU_BASE      0x01c00000UL
#define GPU_REG(dw)   (GPU_BASE + 4UL * (dw))

static int gdsc_on(uintptr_t gdscr, const char *name)
{
	uint32_t v = rd(gdscr);
	con_puts(name); con_puts(" WAS="); con_hex(v, 8);
	if (!(v & (1U << 31))) {
		wr(gdscr, v & ~1U);                                  /* clear SW_COLLAPSE */
		for (int i = 0; i < 1000 && !(rd(gdscr) & (1U << 31)); i++)
			delay_us(10);
	}
	v = rd(gdscr);
	con_puts(" NOW="); con_hex(v, 8); con_puts((v & (1U << 31)) ? " ON\n" : " STILL OFF\n");
	return (v & (1U << 31)) ? 0 : -1;
}

/*
 * GX rail: gdsc_oxili_gx's parent supply is msm_gfx_ldo (qcom,msm8953-gfx-ldo,
 * 0x0185f000). At init-corner 4 (ldo-enable-corner-map 0) it runs in BHS mode: a
 * head switch feeds VDD_CX straight to the GX domain. Without it the GX GDSC
 * never reports PWR_ON (run: GX 0x08225801 -> 0x08225800 STILL OFF).
 * PWRSWITCH_CTRL +0x1c: b0 BHS_UNDER_SW_CTL, b1 BHS_EN_FEW, b2 BHS_EN_REST,
 * b4 LDO_UNDER_SW_CTRL, b5 LDO_PDN, b6 LDO_BYPASS, b31 LDO_CLAMP_IO.
 * PWRSWITCH_STATUS +0x28: b1 BHS_EN_REST_ACK. (Linux msm_gfx_ldo.c layout.)
 */
#define GFX_LDO       0x0185f000UL
#define LDO_CTRL      (GFX_LDO + 0x1c)
#define LDO_STATUS    (GFX_LDO + 0x28)
#define GX_DOMAIN_MISC 0x0185b00cUL
#define MEM_ACC_SEL   0x0194415cUL

/* one PWRSWITCH_CTRL step per write, as the Linux driver does */
static uint32_t ldo_step(uint32_t c)
{
	wr(LDO_CTRL, c);
	__asm__ volatile("dsb sy");
	delay_us(1);
	return c;
}

/*
 * Follows Linux msm_gfx_ldo.c (msm8953): msm_gfx_ldo_init (unclamp, CPR auto-bypass,
 * msm8953_ldo_config table) then enable_bhs_mode: the BHS is handed to the GX GDSC,
 * which sequences it itself when SW_COLLAPSE is cleared. Run 2/3 drove the BHS from
 * software (b0 set) instead, and the GDSC never got its ack.
 */
static void gfx_ldo_bhs(void)
{
	con_puts(" GFX LDO:");
	for (unsigned o = 0; o <= 0x2c; o += 4) {
		con_putc(' '); con_hex(rd(GFX_LDO + o), 8);
	}
	con_puts("\n MEMACC="); con_hex(rd(MEM_ACC_SEL), 8);

	/* msm_gfx_ldo_init: clear clamp_io, enable CPR in auto-bypass, config table */
	uint32_t c = rd(LDO_CTRL);
	c &= ~(1U << 31);
	c |= 1U << 29;
	ldo_step(c);
	wr(GFX_LDO + 0x00, 0x00000203);              /* LDO_ATEST */
	wr(GFX_LDO + 0x04, 0x05008600);              /* LDO_CFG0 */
	wr(GFX_LDO + 0x08, 0x00000000);              /* LDO_CFG1 */
	wr(GFX_LDO + 0x0c, 0x0000c3fc);              /* LDO_CFG2 */
	wr(GFX_LDO + 0x14, 0x004b1102);              /* LDO_VREF_TEST_CFG */
	__asm__ volatile("dsb sy");

	/* enable_bhs_mode (CORNER ops) */
	c = rd(LDO_CTRL);
	c = ldo_step(c | (1U << 4));                 /* LDO under SW control */
	c = ldo_step(c & ~(1U << 9));                /* power source = BHS */
	c = ldo_step(c & ~(1U << 30));               /* clear GFX CPR bypass */
	c = ldo_step(c & ~(1U << 1));                /* BHS_EN_FEW -> GDSC */
	c = ldo_step(c & ~(1U << 2));                /* BHS_EN_REST -> GDSC */
	c = ldo_step(c & ~1U);                       /* BHS under GDSC control */
	con_puts(" BHS->GDSC CTRL="); con_hex(rd(LDO_CTRL), 8);
	con_puts(" S1="); con_hex(rd(GFX_LDO + 0x24), 8);
	con_puts(" ST="); con_hex(rd(LDO_STATUS), 8); con_putc('\n');
}

static uint64_t scm_ret1;
void shim_flush_log(void);

/* TZ SCM call (Qualcomm ARMv8 convention): returns x0, keeps x1 (desc.ret[0]). */
static uint64_t scm_restore(uint64_t fid)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = 2;          /* SCM_ARGS(2) */
	register uint64_t x2 __asm__("x2") = 18;         /* TZ_DEVICE_GPU */
	register uint64_t x3 __asm__("x3") = 0;          /* spare */
	register uint64_t x4 __asm__("x4") = 0;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	__asm__ volatile("dsb sy\n\tsmc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
	                 :
	                 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	scm_ret1 = x1;
	return x0;
}

/* Fault-tolerant read: shim.c's exception handler skips an aborting load while
 * gpu_probe_active is set and flags it in gpu_probe_fault. */
volatile int gpu_probe_active, gpu_probe_fault;

static void show(const char *name, uintptr_t a)
{
	uint32_t v = 0;
	gpu_probe_fault = 0;
	gpu_probe_active = 1;
	__asm__ volatile("dsb sy\n\tldr %w0, [%1]\n\tdsb sy" : "+r"(v) : "r"(a) : "memory");
	gpu_probe_active = 0;
	con_puts(name);
	if (gpu_probe_fault) {
		con_puts("=FAULT");
	} else {
		con_putc('='); con_hex(v, 8);
	}
}

int gpu_probe(void)
{
	con_puts("\nGPU STAGE 1 (A506)\n GCC GFX:");
	for (unsigned o = 0; o <= 0x50; o += 4) {
		if ((o & 0x1f) == 0)
			con_puts("\n  ");
		con_hex(rd(GCC_GFX + o), 8); con_putc(' ');
	}
	con_putc('\n');

	/* gfx3d RCG: XO (src 0) /1 = 19.2 MHz, update, root enable */
	wr(GFX3D_CFG, 0);
	wr(GFX3D_CMD, rd(GFX3D_CMD) | 1U | (1U << 1));
	for (int i = 0; i < 1000 && (rd(GFX3D_CMD) & 1U); i++)
		delay_us(1);
	/* enable every branch CBCR in the gfx block that looks like one (bit 31 CLK_OFF) */
	for (unsigned o = 0x20; o <= 0x48; o += 4) {
		uintptr_t a = GCC_GFX + o;
		if (a == GX_GDSCR || a == CX_GDSCR)
			continue;
		uint32_t v = rd(a);
		if ((v & 0x80000000U) && !(v & 0x7fff0000U))
			wr(a, v | 1U);
	}
	delay_us(100);

	if (gdsc_on(CX_GDSCR, " CX GDSC")) {
		con_puts(" CX FAILED\n");
		return -1;
	}
	/*
	 * Run 7: with CX+GX on and every clock matching Linux, all GPU and GPU-SMMU
	 * reads still took external aborts. Linux arm-smmu (qcom,enable-static-cb +
	 * enable-smmu-halt) calls scm_restore_sec_cfg(sec_id, 0) whenever the CX GDSC
	 * is enabled: TZ re-programs the GPU access control lost while CX was off.
	 * SCM_SIP_FNID(SCM_SVC_MP 0xc, RESTORE_SEC_CFG 2) | SMC64 = 0x42000c02,
	 * x1 = SCM_ARGS(2) = 2, x2 = TZ_DEVICE_GPU (18), x3 = spare 0.
	 */
	{
		con_puts(" TZ RESTORE_SEC_CFG(GPU): CALLING\n");
		shim_flush_log();
		uint64_t r = scm_restore(0x42000c02ULL);
		con_puts(" TZ RESTORE_SEC_CFG(GPU) SMC64 RET="); con_hex(r, 8);
		if (r) {                                    /* fall back to the SMC32 convention */
			r = scm_restore(0x02000c02ULL);
			con_puts(" SMC32 RET="); con_hex(r, 8);
		}
		con_puts(" SCM_RET="); con_hex(scm_ret1, 8); con_putc('\n');
	}
	/* Linux msm_gcc_gfx_probe: "Oxili Ocmem in GX rail: OXILI_GMEM_CLAMP_IO" -
	 * GX_DOMAIN_MISC (GCC 0x5b00c) bit 0 cleared once at boot. Never done before run 6. */
	con_puts(" GX_DOMAIN_MISC WAS="); con_hex(rd(GX_DOMAIN_MISC), 8);
	wr(GX_DOMAIN_MISC, rd(GX_DOMAIN_MISC) & ~1U);
	__asm__ volatile("dsb sy");
	con_puts(" NOW="); con_hex(rd(GX_DOMAIN_MISC), 8); con_putc('\n');
	gfx_ldo_bhs();
	if (gdsc_on(GX_GDSCR, " GX GDSC")) {
		/*
		 * Run 4: the Linux hand-over (BHS under GDSC control) left PWRSWITCH_STATUS
		 * 0 and GX in its waiting state. Fallback: collapse GX again, power the BHS
		 * from software and give the GDSC the fake ack (ACK_SW_OVR, b8), as
		 * switch_mode_to_bhs does during a mode switch, then retry.
		 */
		wr(GX_GDSCR, rd(GX_GDSCR) | 1U);
		delay_us(100);
		uint32_t c = rd(LDO_CTRL);
		c = ldo_step(c | 1U);                    /* BHS under SW control */
		c = ldo_step(c & ~(1U << 9));            /* power source = BHS */
		c = ldo_step(c | (1U << 1));             /* BHS_EN_FEW */
		delay_us(5);
		c = ldo_step(c | (1U << 2));             /* BHS_EN_REST */
		delay_us(50);
		c = ldo_step(c | (1U << 8));             /* fake ack to the GDSC */
		con_puts(" SW BHS+ACK CTRL="); con_hex(rd(LDO_CTRL), 8);
		con_puts(" S1="); con_hex(rd(GFX_LDO + 0x24), 8);
		con_puts(" ST="); con_hex(rd(LDO_STATUS), 8); con_putc('\n');
	}
	if (gdsc_on(GX_GDSCR, " GX GDSC")) {
		con_puts(" GCC GFX NOW:");
		for (unsigned o = 0; o <= 0x50; o += 4) {
			if ((o & 0x1f) == 0)
				con_puts("\n  ");
			con_hex(rd(GCC_GFX + o), 8); con_putc(' ');
		}
		con_puts("\n GPU POWER FAILED - NOT TOUCHING ITS REGISTERS\n");
		return -1;
	}
	/* Run 5: GX said ON (fake ack) but gfx3d CBCR still had CLK_OFF and the first
	 * GPU read took a synchronous external abort (FAR 0x01c013d4). Only touch the
	 * GPU once its core clock really runs. */
	delay_us(100);
	if (rd(GCC_GFX + 0x20) & (1U << 31)) {
		con_puts(" GFX3D CBCR="); con_hex(rd(GCC_GFX + 0x20), 8);
		con_puts(" STILL CLK_OFF - GX NOT REALLY POWERED, NOT READING THE GPU\n");
		return -1;
	}
	con_puts(" GCC GFX AFTER:");
	for (unsigned o = 0x20; o <= 0x48; o += 4) {
		con_putc(' '); con_hex(rd(GCC_GFX + o), 8);
	}
	con_putc('\n');

	/* A5xx registers (dword offsets, Mesa/Linux a5xx.xml). Run 6: RBBM_STATUS took a
	 * synchronous external abort even with the core clock running, so every read now
	 * goes through show(): an abort prints FAULT instead of killing the test. */
	show(" RBBM_CFG0+0", GPU_REG(0x000));
	show(" RBBM_STATUS", GPU_REG(0x4f5));
	show(" RBBM_INT0", GPU_REG(0x4e1));
	con_putc('\n');
	show(" CP_HW_FAULT", GPU_REG(0x821));
	show(" PERF_CP0", GPU_REG(0x3a0));
	show(" VSC_BIN", GPU_REG(0xbc2));
	con_putc('\n');
	show(" GPU+3E000", GPU_BASE + 0x3e000);
	show(" SMMU+0", 0x01c40000UL);
	con_putc('\n');
	con_puts("\n GPU STAGE 1 DONE\n");
	return 0;
}
