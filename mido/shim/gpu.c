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

static uint64_t scm_ret1, scm_ret2;
void shim_flush_log(void);

/* TZ SCM call (Qualcomm ARMv8 convention): returns x0, keeps x1 (desc.ret[0]). */
/* Generic Qualcomm SCM call (ARMv8 SMC64 convention): x0 = fid, x1 = arginfo, x2.. = args. */
static uint64_t scm_call(uint64_t fid, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = arginfo;
	register uint64_t x2 __asm__("x2") = a0;
	register uint64_t x3 __asm__("x3") = a1;
	register uint64_t x4 __asm__("x4") = a2;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	__asm__ volatile("dsb sy\n\tsmc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
	                 :
	                 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	scm_ret1 = x1;
	scm_ret2 = x2;
	return x0;
}

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

static int g_zap_ok;   /* zap shader loaded: SECVID registers are secure-only from then on */

static void cp_state(const char *when)
{
	con_puts(when);
	con_puts(" RBBM_ST="); con_hex(gr(R_RBBM_STATUS), 8);
	con_puts(" RPTR="); con_dec(gr(R_CP_RB_RPTR));
	con_puts(" WPTR="); con_dec(gr(R_CP_RB_WPTR));
	con_puts(" HWF="); con_hex(gr(R_CP_HW_FAULT), 8);
	con_puts(" CPINT="); con_hex(gr(R_CP_INTERRUPT_STATUS), 8);
	con_puts(" RBBMINT="); con_hex(gr(R_RBBM_INT_0_STATUS), 8);
	/* Run 13 (red screen, FAR 0x01c3d000 = 0xf400): once the zap shader is loaded TZ makes
	 * the SECVID registers secure-only; a non-secure read is a synchronous external abort. */
	if (!g_zap_ok) {
		con_puts(" TRUST="); con_hex(gr(R_SECVID_TRUST_CNTL), 8);
	}
	con_putc('\n');
}

static void copy_fw(uintptr_t dst, uintptr_t src, uint32_t len)
{
	for (uint32_t i = 4; i + 3 < len; i += 4)                 /* skip the version dword */
		wr(dst + i - 4, rd(src + i));
}

static int zap_load(void);
static uintptr_t g_blob, g_blob_end;   /* "ZAP1" part of the MIDOGPU1 ramdisk */
static uint32_t g_wptr;      /* ring dwords used by stage 2 (the CP keeps running) */
static uintptr_t g_cb0;

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
	g_blob = lo + 16 + pm4_len + pfp_len;
	g_blob_end = hi;
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
	/* UCHE (run 3: blits executed but wrote nothing; the CP's own MEM_WRITE bypasses UCHE).
	 * a5xx_hw_init: trap/write-through base out of reach, GMEM range (A506 GMEM 128 KB). */
	gw(0x0e96, 2);                                 /* UCHE_CACHE_WAYS */
	gw64(0x0e89, 0x0001ffffffff0000ULL);           /* UCHE_TRAP_BASE */
	gw64(0x0e87, 0x0001ffffffff0000ULL);           /* UCHE_WRITE_THRU_BASE */
	gw64(0x0e8b, 0x00100000);                      /* UCHE_GMEM_RANGE_MIN */
	gw64(0x0e8d, 0x00100000 + 0x20000 - 1);        /* UCHE_GMEM_RANGE_MAX */
	gw(0x046f, 0xffffffff);                        /* RBBM_PERFCTR_GPU_BUSY_MASKED */
	gw(0x0d00, (0x100u << 11) | (0x100u << 22));   /* PC_DBG_ECO_CNTL (A506 values) */
	gw(0x0e81, 1u << 29);                          /* UCHE_MODE_CNTL */
	gw(0x0e60, gr(0x0e60) | (1u << 10));           /* VPC_DBG_ECO_CNTL */
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
	/* Linux a5xx_hw_init puts every block in 64-bit address mode (run 4 had only CP + TSB). */
	gw(R_CP_ADDR_MODE_CNTL, 1);
	gw(0x0bc1, 1); gw(0x0c81, 1); gw(0x0cc5, 1); gw(0x0d01, 1);   /* VSC GRAS RB PC */
	gw(0x0e05, 1); gw(0x0e41, 1); gw(0x0e61, 1); gw(0x0e80, 1);   /* HLSQ VFD VPC UCHE */
	gw(0x0ec1, 1); gw(0x0f01, 1);                                 /* SP TPL1 */
	gw(R_SECVID_TSB_ADDR_MODE, 1);
	gw64(R_CP_ME_INSTR_BASE, G_PM4);
	gw64(R_CP_PFP_INSTR_BASE, G_PFP);
	gw64(R_CP_RB_BASE, G_RING);
	gw(R_CP_RB_CNTL, 11u | (2u << 8) | (1u << 27));   /* 16 KB ring, 32 B blocks, no rptr writeback */
	cp_state(" BEFORE ME START:");

	/* Run 10: AUTH_AND_RESET failed (-2/-15) with the CP already running. kgsl loads the
	 * zap shader in a5xx_microcode_load, before the ME/PFP halt is released: do the same.
	 * It must also come before the ring is built (the secure-mode exit depends on it). */
	g_zap_ok = zap_load() == 0;
	con_puts(g_zap_ok ? " ZAP SHADER LOADED\n" : " ZAP SHADER FAILED\n");

	uint32_t n = 0;
	uintptr_t rb = G_RING;
#define RB(v) do { wr(rb + 4u * n, (v)); n++; } while (0)
	RB(pkt7(CP_ME_INIT, 8));
	RB(0x2f); RB(0x3); RB(0x20000000); RB(0); RB(0); RB(0); RB(0); RB(0);
	/* Run 13: with the zap shader loaded the GPU starts in secure mode, and the first
	 * CP_MEM_WRITE to normal memory reset the SoC (XPU). Linux sends CP_SET_SECURE_MODE 0
	 * right after CP_ME_INIT when the zap is loaded; without the zap it is an opcode error. */
	if (g_zap_ok) {
		RB(pkt7(0x66, 1)); RB(0);                    /* CP_SET_SECURE_MODE 0 */
		RB(pkt7(0x26, 0));                           /* CP_WAIT_FOR_IDLE */
	}
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
	g_wptr = n;
	g_cb0 = cb0;
	return got == 0xc0ffee01u ? 0 : -1;
}

/*
 * GPU stage 3: the GPU draws something visible. The CPU draws a 256x256 gradient
 * off screen (0xa0100000, pitch 1024); the GPU's 2D engine copies it into the
 * framebuffer (0x90001000, pitch 4320) row by row (2D surface bases must be
 * 64-byte aligned and the pitch a multiple of 64, which 4320 is not), from an
 * indirect buffer at 0xa0200000. Sequence after Mesa freedreno fd5_blitter.c
 * (emit_setup + emit_blit_buffer) and fd5_gmem.c (CCU colour flush + cache flush);
 * offsets from Mesa a5xx.xml / adreno_pm4.xml.
 */
#define R_RB_RENDER_CNTL     0xe141
#define R_RB_2D_BLIT_CNTL    0x2100
#define R_GRAS_2D_BLIT_CNTL  0x2180
#define R_UNKNOWN_2184       0x2184
#define R_RB_CNTL            0xe140
#define R_RB_MODE_CNTL       0x0cc6
#define R_SP_MODE_CNTL       0x0ec2
#define R_TPL1_MODE_CNTL     0x0f02
#define R_HLSQ_MODE_CNTL     0x0e06
#define R_GRAS_CL_CNTL       0xe000
#define R_RB_2D_SRC_INFO     0x2107
#define R_RB_2D_DST_INFO     0x2110
#define R_GRAS_2D_SRC_INFO   0x2181
#define R_GRAS_2D_DST_INFO   0x2182
#define CP_WAIT_FOR_IDLE     0x26
#define CP_BLIT              0x2c
#define CP_INDIRECT_BUFFER   0x3f
#define CP_EVENT_WRITE       0x46
#define CP_SET_RENDER_MODE   0x6c
#define EV_CACHE_FLUSH_TS    0x04
#define EV_CCU_FLUSH_COLOR   0x1d
#define FMT_RGBA8            0x30           /* RB5_R8G8B8A8_UNORM, TILE5_LINEAR, WZYX */

#define G_SRC   0xa0100000UL
#define G_IB    0xa0200000UL
#define G_DST2  0xa0300000UL
#define FB      0x90001000UL
#define FB_PITCH 4320u

static uint32_t pkt4(uint32_t reg, uint32_t cnt)
{
	return (4u << 28) | cnt | (par(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (par(reg) << 27);
}

/*
 * Zap shader through TZ PIL (Linux qcom_mdt_load + qcom_scm_pas_*, kgsl-hyp node):
 * pas-id 13, firmware a506_zap: .mdt (ELF32 headers + hash segment = the metadata)
 * and .b02 (the only PT_LOAD, paddr 0x5000, 0x1444 bytes, relocatable). The carveout
 * follows gpu_region (alloc 0x80000000..0x90000000, 4 MB aligned): 0x8f000000. TZ checks
 * the signature with the crypto engine, so its clocks are voted on first.
 * SCM: SIP svc PIL (2): INIT_IMAGE 1 (pas, metadata RW), MEM_SETUP 2 (pas, addr, size),
 * AUTH_AND_RESET 5 (pas). Run 7: CP_SET_SECURE_MODE without it = CP opcode error.
 */
#define ZAP_PAS     13
#define ZAP_CARVE   0x8f800000UL   /* the address Linux used (OrangeFox: "loading from 0x8f800000") */
#define ZAP_META    ZAP_CARVE      /* msm pil_boot passes region_start as mdata_phys */
#define ZAP_SIZE    0x00800000UL

/* Clean + invalidate the data cache by address (works with the MMU off: VA = PA). LK ran
 * with caches on; TZ reads cacheable, the shim writes uncached. Run 11: AUTH_AND_RESET -15
 * even with the CP halted, so make sure no stale line shadows what the shim wrote. */
static void dc_civac(uintptr_t a, uintptr_t len)
{
	for (uintptr_t x = a & ~63UL; x < a + len; x += 64)
		__asm__ volatile("dc civac, %0" :: "r"(x) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int zap_load(void)
{
	if (!g_blob || g_blob + 12 > g_blob_end || rd(g_blob) != 0x3150415au) {   /* "ZAP1" */
		con_puts(" NO ZAP1 IN THE RAMDISK\n");
		return -1;
	}
	uint32_t mdt_len = rd(g_blob + 4), seg_len = rd(g_blob + 8);
	uintptr_t mdt = g_blob + 12, seg = mdt + mdt_len;
	if (seg + seg_len > g_blob_end || mdt_len > 0x10000 || seg_len > 0x100000) {
		con_puts(" BAD ZAP1 SIZES\n");
		return -1;
	}
	/* crypto engine clocks: GCC APCS_CLOCK_BRANCH_ENA_VOTE b0 ahb, b1 axi, b2 core */
	wr(0x01845004UL, rd(0x01845004UL) | 7u);
	delay_us(100);
	dc_civac(ZAP_CARVE, 0x10000);
	for (uint32_t i = 0; i + 3 < mdt_len; i += 4)
		wr(ZAP_META + i, rd(mdt + i));
	dc_civac(ZAP_META, mdt_len);
	con_puts(" ZAP: MDT "); con_dec(mdt_len); con_puts(" B, SEG "); con_dec(seg_len); con_puts(" B\n");
	/* Run 12: Linux (OrangeFox) loads this zap fine ("Brought out of reset" at 0x8f800000),
	 * the shim gets AUTH -15 with identical inputs. Missing boot-time step: arm-smmu
	 * register_iommu_sec_ptbl -> msm_iommu_sec_pgtbl_init gives TZ memory for the secure
	 * SMMU page tables (MP svc: PTBL_SIZE 3, PTBL_INIT 4 (paddr RW, size, spare)). */
	{
		uint64_t r0 = scm_call(0x42000c03ULL, 1, 0, 0, 0);
		uint64_t psize = scm_ret1, pfail = scm_ret2;
		con_puts(" SEC PTBL SIZE RET="); con_hex(r0, 8); con_puts(" SIZE="); con_hex(psize, 8);
		con_puts(" FAIL="); con_hex(pfail, 8); con_putc('\n');
		if (!r0 && !pfail && psize && psize <= 0x2000000u) {
			for (uint64_t i = 0; i < psize; i += 4)
				wr(0x8c000000UL + i, 0);
			dc_civac(0x8c000000UL, psize);
			con_puts(" SEC PTBL INIT\n");
			shim_flush_log();
			r0 = scm_call(0x42000c04ULL, 0x23, 0x8c000000UL, psize, 0);   /* SCM_ARGS(3, RW, VAL, VAL) */
			con_puts(" SEC PTBL INIT RET="); con_hex(r0, 8); con_puts(" TZ="); con_hex(scm_ret1, 8);
			con_putc('\n');
		}
	}
	con_puts(" ZAP: INIT_IMAGE\n");
	shim_flush_log();
	uint64_t r = scm_call(0x42000201ULL, 0x82, ZAP_PAS, ZAP_META, 0);   /* SCM_ARGS(2, VAL, RW) */
	con_puts(" ZAP: INIT_IMAGE RET="); con_hex(r, 8); con_puts(" TZ="); con_hex(scm_ret1, 8); con_putc('\n');
	if (r || scm_ret1)
		return -2;
	/* Run 8: MEM_SETUP with the whole 8 MB passed but AUTH_AND_RESET failed (-2 / -15).
	 * msm pil.c declares only the image span: lowest to highest PT_LOAD paddr + memsz,
	 * from the ELF32 program headers in the .mdt (relocatable: loaded at carve + paddr - min). */
	uint32_t phoff = rd(ZAP_META + 28), phent = rd(ZAP_META + 40) >> 16, phnum = rd(ZAP_META + 44) & 0xffffu;
	uint32_t lo = 0xffffffffu, hi = 0;
	for (uint32_t i = 0; i < phnum && phoff + (i + 1) * phent <= mdt_len; i++) {
		uintptr_t ph = ZAP_META + phoff + i * phent;
		uint32_t type = rd(ph), paddr = rd(ph + 12), memsz = rd(ph + 20), flags = rd(ph + 24);
		if (type != 1 || memsz == 0 || ((flags >> 24) & 7u) == 2u)   /* PT_LOAD, not the hash */
			continue;
		if (paddr < lo) lo = paddr;
		if (paddr + memsz > hi) hi = paddr + memsz;
	}
	if (hi <= lo) {
		con_puts(" ZAP: NO PT_LOAD IN THE MDT\n");
		return -3;
	}
	con_puts(" ZAP: IMAGE "); con_hex(lo, 8); con_puts(".."); con_hex(hi, 8);
	con_puts(" SPAN "); con_hex(hi - lo, 8); con_putc('\n');
	/* Run 9: MEM_SETUP with the raw span 0x1444 was rejected (-16). msm pil_setup_region
	 * rounds the end up to 4 KB: region_end - region_start = ALIGN(hi, 4K) - lo. */
	hi = (hi + 0xfffu) & ~0xfffu;
	r = scm_call(0x42000202ULL, 3, ZAP_PAS, ZAP_CARVE, hi - lo);
	con_puts(" ZAP: MEM_SETUP RET="); con_hex(r, 8); con_puts(" TZ="); con_hex(scm_ret1, 8); con_putc('\n');
	if (r || scm_ret1)
		return -3;
	for (uint32_t i = 0; i < hi - lo; i += 4)          /* the metadata was here too */
		wr(ZAP_CARVE + i, 0);
	for (uint32_t i = 0; i + 3 < seg_len; i += 4)
		wr(ZAP_CARVE + i, rd(seg + i));
	dc_civac(ZAP_CARVE, 0x10000);
	con_puts(" ZAP: AUTH_AND_RESET\n");
	shim_flush_log();
	r = scm_call(0x42000205ULL, 1, ZAP_PAS, 0, 0);
	con_puts(" ZAP: AUTH_AND_RESET RET="); con_hex(r, 8); con_puts(" TZ="); con_hex(scm_ret1, 8); con_putc('\n');
	return (r || scm_ret1) ? -4 : 0;
}

int gpu_stage3(void)
{
	if (!g_zap_ok) {
		con_puts(" ZAP SHADER NOT LOADED IN STAGE 2 - NO SECURE-MODE EXIT, STOP\n");
		return -1;
	}
	con_puts("\nGPU STAGE 3 (2D BLIT TO THE SCREEN)\n");
	if (!g_wptr) {
		con_puts(" STAGE 2 DID NOT PASS - SKIPPED\n");
		return -1;
	}
	/* source: gradient, red = x, green = y, blue = 0xc0 */
	for (uint32_t y = 0; y < 256; y++)
		for (uint32_t x = 0; x < 256; x++)
			wr(G_SRC + y * 1024 + x * 4, (x << 16) | (y << 8) | 0xc0u);

	const uint32_t X0 = 412, Y0 = 1400, H = 256;
	uint32_t k = 0;
#define IB(v) do { wr(G_IB + 4u * k, (v)); k++; } while (0)
	IB(pkt4(0x0e91, 5)); IB(0); IB(0); IB(0); IB(0); IB(0x12);   /* UCHE invalidate */
	IB(pkt7(CP_WAIT_FOR_IDLE, 0));
	/* Run 5: no SMMU fault, no pixels, 257 blits in 9 ms: the blits were dropped. Mesa
	 * fd5_emit_sysmem_prep runs before any blit: render mode BYPASS, IB2 skip off, CCU colour
	 * invalidate, PC/VFD power (A506: one SP core -> 0), and RB_CCU_CNTL 0x10000000
	 * ("for BYPASS"), which configures the colour cache the 2D engine writes through. */
	IB(pkt7(CP_SET_RENDER_MODE, 5)); IB(1); IB(0); IB(0); IB(0); IB(0);   /* BYPASS */
	IB(pkt7(0x1d, 1)); IB(0);                                       /* CP_SKIP_IB2_ENABLE_GLOBAL */
	IB(pkt7(CP_EVENT_WRITE, 1)); IB(0x19);                          /* PC_CCU_INVALIDATE_COLOR */
	IB(pkt4(0xe3b0, 1)); IB(0);                                     /* PC_POWER_CNTL */
	IB(pkt4(0xe4f0, 1)); IB(0);                                     /* VFD_POWER_CNTL */
	IB(pkt7(CP_WAIT_FOR_IDLE, 0));
	IB(pkt4(0x0cc7, 1)); IB(0x10000000);                            /* RB_CCU_CNTL: BYPASS */
	IB(pkt4(R_RB_RENDER_CNTL, 1)); IB(0x00000008);
	IB(pkt4(R_RB_2D_BLIT_CNTL, 1)); IB(0x86000000);
	IB(pkt4(R_GRAS_2D_BLIT_CNTL, 1)); IB(0x86000000);
	IB(pkt4(R_UNKNOWN_2184, 1)); IB(0x00000009);
	IB(pkt4(R_RB_CNTL, 1)); IB(1u << 17);                 /* BYPASS */
	IB(pkt4(R_RB_MODE_CNTL, 1)); IB(0x00000004);
	IB(pkt4(R_SP_MODE_CNTL, 1)); IB(0x0000000c);
	IB(pkt4(R_TPL1_MODE_CNTL, 1)); IB(0x00000344);
	IB(pkt4(R_HLSQ_MODE_CNTL, 1)); IB(0x00000002);
	IB(pkt4(R_GRAS_CL_CNTL, 1)); IB(0x00000181);
	/* Run 4: TS events landed but no pixel changed. Control blit: the whole square in one
	 * CP_BLIT into ordinary RAM (0xa0300000, pitch 1024), checked by the CPU. */
	for (uint32_t i = 0; i < 256 * 256; i++)
		wr(G_DST2 + 4u * i, 0x5a5a5a5au);
	IB(pkt7(CP_SET_RENDER_MODE, 1)); IB(5);
	IB(pkt4(R_RB_2D_SRC_INFO, 9));
	IB(FMT_RGBA8); IB((uint32_t)G_SRC); IB(0); IB((1024 >> 6) | ((1024 * 256 >> 6) << 16));
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(R_GRAS_2D_SRC_INFO, 1)); IB(FMT_RGBA8);
	IB(pkt4(R_RB_2D_DST_INFO, 9));
	IB(FMT_RGBA8); IB((uint32_t)G_DST2); IB(0); IB((1024 >> 6) | ((1024 * 256 >> 6) << 16));
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(R_GRAS_2D_DST_INFO, 1)); IB(FMT_RGBA8);
	IB(pkt7(CP_BLIT, 5)); IB(1); IB(0); IB(255 | (255u << 16)); IB(0); IB(255 | (255u << 16));
	IB(pkt7(CP_SET_RENDER_MODE, 1)); IB(8);
	IB(pkt7(CP_WAIT_FOR_IDLE, 0));
	for (uint32_t y = 0; y < H; y++) {
		uint64_t src = G_SRC + y * 1024;
		uint64_t dst = FB + (uint64_t)(Y0 + y) * FB_PITCH + X0 * 4;
		uint32_t dx = (uint32_t)(dst & 63) / 4;
		dst &= ~63ULL;
		IB(pkt7(CP_SET_RENDER_MODE, 1)); IB(5);           /* BLIT2D */
		IB(pkt4(R_RB_2D_SRC_INFO, 9));
		IB(FMT_RGBA8); IB((uint32_t)src); IB((uint32_t)(src >> 32)); IB((1024 >> 6) | ((1024 >> 6) << 16));   /* pitch | array pitch (1 row) */
		IB(0); IB(0); IB(0); IB(0); IB(0);
		IB(pkt4(R_GRAS_2D_SRC_INFO, 1)); IB(FMT_RGBA8);
		IB(pkt4(R_RB_2D_DST_INFO, 9));
		IB(FMT_RGBA8); IB((uint32_t)dst); IB((uint32_t)(dst >> 32)); IB((4352 >> 6) | ((4352 >> 6) << 16));
		IB(0); IB(0); IB(0); IB(0); IB(0);
		IB(pkt4(R_GRAS_2D_DST_INFO, 1)); IB(FMT_RGBA8);
		IB(pkt7(CP_BLIT, 5));
		IB(1);                                            /* BLIT_OP_COPY */
		IB(0); IB(255);                                   /* src x1,y1=0 / x2=255,y2=0 */
		IB(dx); IB(dx + 255);                             /* dst x1 / x2, y = 0 */
		IB(pkt7(CP_SET_RENDER_MODE, 1)); IB(8);           /* END2D */
		IB(pkt7(CP_WAIT_FOR_IDLE, 0));
	}
	IB(pkt7(CP_EVENT_WRITE, 4)); IB(EV_CCU_FLUSH_COLOR);
	IB((uint32_t)(G_SCRATCH + 0x100)); IB(0); IB(0x11111111u);
	IB(pkt7(CP_EVENT_WRITE, 4)); IB(EV_CACHE_FLUSH_TS);
	IB((uint32_t)(G_SCRATCH + 0x200)); IB(0); IB(0x22222222u);
	IB(pkt7(CP_WAIT_FOR_IDLE, 0));
#undef IB
	con_puts(" IB "); con_dec(k); con_puts(" DWORDS, RING AT "); con_dec(g_wptr); con_putc('\n');

	uint32_t n = g_wptr;
	uintptr_t rb = G_RING;
#define RB(v) do { wr(rb + 4u * n, (v)); n++; } while (0)
	/* The secure-mode exit (CP_SET_SECURE_MODE 0) now happens in stage 2, right after
	 * CP_ME_INIT, as kgsl/Linux do once the zap shader is loaded. */
	RB(pkt7(CP_INDIRECT_BUFFER, 3)); RB((uint32_t)G_IB); RB((uint32_t)(G_IB >> 32)); RB(k);
	RB(pkt7(CP_WAIT_FOR_IDLE, 0));
	RB(pkt7(CP_MEM_WRITE, 3));
	RB((uint32_t)G_SCRATCH); RB((uint32_t)(G_SCRATCH >> 32)); RB(0xc0ffee02u);
#undef RB
	__asm__ volatile("dsb sy" ::: "memory");
	con_puts(" SMMU GFSR BEFORE="); con_hex(rd(SMMU_BASE + 0x48), 8); con_putc('\n');
	con_puts(" KICKING THE BLIT\n");
	shim_flush_log();
	gw(R_CP_RB_WPTR, n);
	unsigned ms;
	for (ms = 0; ms < 2000 && rd(G_SCRATCH) != 0xc0ffee02u; ms++)
		delay_us(1000);
	cp_state(ms < 2000 ? " BLIT DONE:" : " BLIT TIMED OUT (2 S):");
	con_puts(" CB0 FSR="); con_hex(rd(g_cb0 + 0x58), 8);
	con_puts(" MS="); con_dec(ms);
	con_puts(" TS1="); con_hex(rd(G_SCRATCH + 0x100), 8); con_puts(" TS2="); con_hex(rd(G_SCRATCH + 0x200), 8);
	con_puts(" FB PIXEL(0,0)="); con_hex(rd(FB + (uint64_t)Y0 * FB_PITCH + X0 * 4), 8);
	con_puts(" (255,255)="); con_hex(rd(FB + (uint64_t)(Y0 + 255) * FB_PITCH + (X0 + 255) * 4), 8);
	con_putc('\n');
	uint32_t same = 0, untouched = 0;
	for (uint32_t i = 0; i < 256 * 256; i++) {
		uint32_t v = rd(G_DST2 + 4u * i);
		same += v == rd(G_SRC + 4u * i);
		untouched += v == 0x5a5a5a5au;
	}
	con_puts(" SMMU GFSR="); con_hex(rd(SMMU_BASE + 0x48), 8);
	con_puts(" GFAR="); con_hex(rd(SMMU_BASE + 0x44), 8); con_hex(rd(SMMU_BASE + 0x40), 8);
	con_puts(" GFSYNR0="); con_hex(rd(SMMU_BASE + 0x50), 8);
	con_puts(" GFSYNR1="); con_hex(rd(SMMU_BASE + 0x54), 8); con_putc('\n');
	con_puts(" RAM BLIT: "); con_dec(same); con_puts(" OF 65536 PIXELS MATCH, ");
	con_dec(untouched); con_puts(" UNTOUCHED, DST2[0]="); con_hex(rd(G_DST2), 8);
	con_puts(" [1]="); con_hex(rd(G_DST2 + 4), 8); con_putc('\n');
	uint32_t fbv = rd(FB + (uint64_t)Y0 * FB_PITCH + X0 * 4);
	con_puts(fbv == rd(G_SRC) ? " SCREEN BLIT LANDED: THE SQUARE BELOW WAS DRAWN BY THE GPU\n" :
	         same == 65536 ? " RAM BLIT WORKS, SCREEN BLIT BLOCKED (FRAMEBUFFER MEMORY)\n" : " NO BLIT OUTPUT\n");
	g_wptr = n;
	return ms < 2000 ? 0 : -1;
}
