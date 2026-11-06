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

/*
 * GPU stage 2: command processor test (shim only, after gpu_probe() succeeded).
 * Sequence after Linux a5xx_hw_init / a5xx_me_init (register offsets: Mesa
 * a5xx.xml dword offsets; opcodes: adreno_pm4.xml):
 *   - firmware from the test image's ramdisk ("MIDOGPU1" blob made by build.py
 *     from fw/gpu, not in git), copied without its 4-byte header to 0xa0008000 /
 *     0xa0010000; ring at 0xa0000000, scratch at 0xa0004000. The MMU and caches
 *     are off in the shim, so the GPU and the CPU see the same bytes.
 *   - GPU SMMU: TZ owns the global registers (static context banks; reading
 *     +0x400 resets the SoC). Only the non-secure context bank 0 (gfx3d_user)
 *     is read here; with its SCTLR.M = 0 the GPU's addresses pass untranslated.
 *   - CP_ME_INIT, then CP_MEM_WRITE 0xc0ffee01 to scratch, then a NOP.
 */
#include "fdt.h"

#define R_CP_RB_BASE            0x0800
#define R_CP_RB_CNTL            0x0802
#define R_CP_RB_RPTR            0x0806
#define R_CP_RB_WPTR            0x0807
#define R_CP_ROQ_THRESHOLDS_1   0x081f
#define R_CP_ROQ_THRESHOLDS_2   0x0820
#define R_CP_MEQ_THRESHOLDS     0x0825
#define R_CP_MERCIU_SIZE        0x0826
#define R_CP_PFP_ME_CNTL        0x0832
#define R_CP_CHICKEN_DBG        0x0833
#define R_CP_PFP_INSTR_BASE     0x0835
#define R_CP_ME_INSTR_BASE      0x0838
#define R_CP_ADDR_MODE_CNTL     0x0860
#define R_CP_INTERRUPT_STATUS   0x0b18
#define R_CP_HW_FAULT           0x0b1a
#define R_RBBM_AHB_CNTL0        0x0093
#define R_RBBM_AHB_CNTL1        0x0094
#define R_RBBM_AHB_CNTL2        0x0095
#define R_RBBM_INT_0_STATUS     0x04e1
#define R_RBBM_STATUS           0x04f5
#define R_VBIF_RR_QOS_ARB       0x3049
#define R_SECVID_TRUST_CNTL     0xf400
#define R_SECVID_TSB_BASE       0xf800
#define R_SECVID_TSB_SIZE       0xf802
#define R_SECVID_TSB_CNTL       0xf803
#define R_SECVID_TSB_ADDR_MODE  0xf810
#define CP_NOP                  0x10
#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48

#define G_RING     0xa0000000UL
#define G_SCRATCH  0xa0004000UL
#define G_PM4      0xa0008000UL
#define G_PFP      0xa0010000UL
#define SMMU_BASE  0x01c40000UL

static void gw(uint32_t reg, uint32_t v) { wr(GPU_REG(reg), v); }
static uint32_t gr(uint32_t reg) { return rd(GPU_REG(reg)); }
static void gw64(uint32_t reg, uint64_t v) { gw(reg, (uint32_t)v); gw(reg + 1, (uint32_t)(v >> 32)); }

static uint32_t par(uint32_t v)
{
	v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}

/* type-7 packet header (Linux msm_ringbuffer OUT_PKT7) */
static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return (7u << 28) | cnt | (par(cnt) << 15) | ((op & 0x7fu) << 16) | (par(op) << 23);
}

static void cp_state(const char *when)
{
	con_puts(when);
	con_puts(" RBBM_ST="); con_hex(gr(R_RBBM_STATUS), 8);
	con_puts(" RPTR="); con_dec(gr(R_CP_RB_RPTR));
	con_puts(" WPTR="); con_dec(gr(R_CP_RB_WPTR));
	con_puts(" HWF="); con_hex(gr(R_CP_HW_FAULT), 8);
	con_puts(" CPINT="); con_hex(gr(R_CP_INTERRUPT_STATUS), 8);
	con_puts(" RBBMINT="); con_hex(gr(R_RBBM_INT_0_STATUS), 8);
	con_puts(" TRUST="); con_hex(gr(R_SECVID_TRUST_CNTL), 8); con_putc('\n');
}

static void copy_fw(uintptr_t dst, uintptr_t src, uint32_t len)
{
	for (uint32_t i = 4; i + 3 < len; i += 4)                 /* skip the version dword */
		wr(dst + i - 4, rd(src + i));
}

int gpu_stage2(uint64_t dtb)
{
	con_puts("\nGPU STAGE 2 (CP)\n");
	uint64_t lo = 0, hi = 0;
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &lo) || !fdt_chosen_u64(dtb, "linux,initrd-end", &hi) ||
	    hi - lo < 16 || rd(lo) != 0x4f44494du || rd(lo + 4) != 0x31555047u) {   /* "MIDOGPU1" */
		con_puts(" NO MIDOGPU1 BLOB IN THE RAMDISK - NO CP TEST\n");
		return -1;
	}
	uint32_t pm4_len = rd(lo + 8), pfp_len = rd(lo + 12);
	if (16 + (uint64_t)pm4_len + pfp_len > hi - lo || pm4_len > 0x8000 || pfp_len > 0x8000) {
		con_puts(" BAD BLOB SIZES\n");
		return -1;
	}
	copy_fw(G_PM4, lo + 16, pm4_len);
	copy_fw(G_PFP, lo + 16 + pm4_len, pfp_len);
	for (uintptr_t a = G_RING; a < G_PM4; a += 4)
		wr(a, 0);
	con_puts(" PM4 "); con_dec(pm4_len); con_puts(" B VER="); con_hex(rd(G_PM4), 8);
	con_puts(" PFP "); con_dec(pfp_len); con_puts(" B VER="); con_hex(rd(G_PFP), 8); con_putc('\n');

	/* SMMU: global ID registers (non-secure view) and context bank 0 only */
	uint32_t idr1 = rd(SMMU_BASE + 0x24);
	uint32_t pgsz = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	uint32_t npages = 1u << (((idr1 >> 28) & 7u) + 1);
	uintptr_t cb0 = SMMU_BASE + (uintptr_t)npages * pgsz;
	con_puts(" SMMU CR0="); con_hex(rd(SMMU_BASE), 8);
	con_puts(" IDR1="); con_hex(idr1, 8);
	con_puts(" CB0@"); con_hex(cb0, 8);
	if (cb0 + 0x1000 > SMMU_BASE + 0x10000) {
		con_puts(" OUTSIDE THE SMMU WINDOW - STOP\n");
		return -1;
	}
	con_puts("\n CB0 SCTLR="); con_hex(rd(cb0 + 0x0), 8);
	con_puts(" TCR="); con_hex(rd(cb0 + 0x30), 8);
	con_puts(" TTBR0="); con_hex(rd(cb0 + 0x24), 8); con_hex(rd(cb0 + 0x20), 8);
	con_puts(" FSR="); con_hex(rd(cb0 + 0x58), 8); con_putc('\n');
	if (rd(cb0) & 1u) {
		/* translation on: turn this context bank's stage 1 off so PA = VA */
		wr(cb0, rd(cb0) & ~1u);
		con_puts(" CB0 SCTLR.M CLEARED -> "); con_hex(rd(cb0), 8); con_putc('\n');
	}

	/* subset of a5xx_hw_init */
	gw(R_VBIF_RR_QOS_ARB, 3);
	gw(R_RBBM_AHB_CNTL0, 1);
	gw(R_CP_MEQ_THRESHOLDS, 0x20);
	gw(R_CP_MERCIU_SIZE, 0x20);
	gw(R_CP_ROQ_THRESHOLDS_2, 0x40000030);
	gw(R_CP_ROQ_THRESHOLDS_1, 0x20100d0a);
	gw(R_CP_CHICKEN_DBG, 0x02000000);
	gw(R_RBBM_AHB_CNTL1, 0xa6ffffff);
	gw(R_RBBM_AHB_CNTL2, 0x3f);
	gw(R_SECVID_TSB_CNTL, 0);
	gw64(R_SECVID_TSB_BASE, 0);
	gw(R_SECVID_TSB_SIZE, 0);
	gw(R_CP_ADDR_MODE_CNTL, 1);
	gw(R_SECVID_TSB_ADDR_MODE, 1);
	gw64(R_CP_ME_INSTR_BASE, G_PM4);
	gw64(R_CP_PFP_INSTR_BASE, G_PFP);
	gw64(R_CP_RB_BASE, G_RING);
	gw(R_CP_RB_CNTL, 11u | (2u << 8) | (1u << 27));   /* 16 KB ring, 32 B blocks, no rptr writeback */
	cp_state(" BEFORE ME START:");

	uint32_t n = 0;
	uintptr_t rb = G_RING;
#define RB(v) do { wr(rb + 4u * n, (v)); n++; } while (0)
	RB(pkt7(CP_ME_INIT, 8));
	RB(0x2f); RB(0x3); RB(0x20000000); RB(0); RB(0); RB(0); RB(0); RB(0);
	RB(pkt7(CP_MEM_WRITE, 3));
	RB((uint32_t)G_SCRATCH); RB((uint32_t)(G_SCRATCH >> 32)); RB(0xc0ffee01u);
	RB(pkt7(CP_NOP, 0));
#undef RB
	__asm__ volatile("dsb sy" ::: "memory");
	con_puts(" STARTING THE CP\n");
	shim_flush_log();

	/* Run 1 reset the SoC somewhere after this point: split into steps, log each. */
	gw(R_CP_PFP_ME_CNTL, 0);                       /* clear the ME/PFP halt: microcode fetch */
	delay_us(5000);
	cp_state(" A HALT CLEARED:");
	con_puts(" A CB0 FSR="); con_hex(rd(cb0 + 0x58), 8); con_putc('\n');
	shim_flush_log();
	gw(R_CP_RB_WPTR, n);                           /* ring fetch */
	delay_us(2000);
	cp_state(" B WPTR WRITTEN:");
	shim_flush_log();
	unsigned ms;
	for (ms = 0; ms < 500 && gr(R_CP_RB_RPTR) != n; ms++)
		delay_us(1000);
	delay_us(2000);
	uint32_t got = rd(G_SCRATCH);
	cp_state(ms < 500 ? " RING CONSUMED:" : " RING NOT CONSUMED (500 MS):");
	con_puts(" CB0 FSR="); con_hex(rd(cb0 + 0x58), 8);
	con_puts(" FAR="); con_hex(rd(cb0 + 0x64), 8); con_hex(rd(cb0 + 0x60), 8); con_putc('\n');
	con_puts(" SCRATCH="); con_hex(got, 8);
	con_puts(got == 0xc0ffee01u ? " -> CP_MEM_WRITE LANDED: COMMAND PROCESSOR WORKS\n" : " -> NO WRITE\n");
	return got == 0xc0ffee01u ? 0 : -1;
}
