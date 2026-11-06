/*
 * mido (Redmi Note 4, MSM8953) Adreno 506 bring-up inside XNU, step 1 of GPU acceleration.
 *
 * The same sequence the shim proved on hardware (mido/shim/gpu.c, PORTING_LOG.md
 * "GPU stage 1-3"), run once from a kernel thread when the boot-arg midogpu=1 is set:
 *   1. GCC gfx clocks (gfx3d RCG at XO), CX GDSC, TZ restore_sec_cfg(GPU = 18),
 *      GX_DOMAIN_MISC GMEM clamp release, gfx LDO handed to the GX GDSC (BHS), GX GDSC.
 *   2. a5xx_hw_init subset, CP microcode (a530 pm4/pfp), TZ secure page-table pool,
 *      zap shader through TZ PIL (pas-id 13), ME_INIT + CP_SET_SECURE_MODE 0.
 *   3. Self-test: CP_MEM_WRITE to a scratch word, then one 2D CP_BLIT of a 256x256
 *      gradient into ordinary memory, checked by the CPU.
 * Firmware comes from the payload (mkpayload --blob GPU-PM4/GPU-PFP/GPU-ZAP-MDT/
 * GPU-ZAP-SEG, files from the phone, never in git), announced in /chosen/memory-map.
 *
 * Memory: fixed physical windows between the splash buffer (ends 0x91400000) and
 * pstore (0x9ff00000), outside XNU's RAM (0xa0000000+), mapped uncached with
 * ml_io_map so the GPU and the CPU always agree:
 *   0x98000000  TZ secure SMMU page-table pool (TZ owns it afterwards: never touched)
 *   0x98800000  zap shader region (TZ owns it afterwards: never touched)
 *   0x99000000  ring 16 KB | 0x99004000 scratch | 0x99008000 pm4 | 0x99010000 pfp
 *   0x99100000  blit source | 0x99200000 IB | 0x99300000 blit destination
 * All MMIO and these windows are accessed 32 bits at a time (device memory).
 * Register offsets: Mesa a5xx.xml / adreno_pm4.xml; sequences: Linux msm drm a5xx,
 * msm_gfx_ldo.c, gdsc.c, peripheral-loader.c, subsys-pil-tz.c, io-pgtable-msm-secure.c.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_static_ptovirt(vm_offset_t);
extern kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);

/* physical layout */
#define P_GCC        0x01840000UL       /* 0x20000: APCS vote 0x45004, gfx 0x59000, misc 0x5b00c, ldo 0x5f000 */
#define P_GPU        0x01c00000UL       /* 0x40000 registers + SMMU at +0x40000 (0x10000) */
#define P_PTBL       0x98000000UL
#define P_ZAP        0x98800000UL
#define P_WORK       0x99000000UL
#define WORK_SIZE    0x00400000UL

static vm_offset_t gcc, gpu, smmu, work, zapw, ptblw;

#define GCC(off)     (gcc + ((off) - P_GCC))
#define GFX3D_CMD    GCC(0x01859000UL)
#define GFX3D_CFG    GCC(0x01859004UL)
#define GX_GDSCR     GCC(0x0185901cUL)
#define CX_GDSCR     GCC(0x0185904cUL)
#define GX_DOM_MISC  GCC(0x0185b00cUL)
#define LDO(o)       GCC(0x0185f000UL + (o))
#define APCS_VOTE    GCC(0x01845004UL)

#define W(o)         (work + (o))
#define O_RING       0x000000u
#define O_SCRATCH    0x004000u
#define O_PM4        0x008000u
#define O_PFP        0x010000u
#define O_SRC        0x100000u
#define O_IB         0x200000u
#define O_DST        0x300000u

static inline uint32_t rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
static inline void wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void gw(uint32_t reg, uint32_t v) { wr(gpu + 4u * reg, v); }
static inline uint32_t gr(uint32_t reg) { return rd(gpu + 4u * reg); }
static void gw64(uint32_t reg, uint64_t v) { gw(reg, (uint32_t)v); gw(reg + 1, (uint32_t)(v >> 32)); }
static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t scm_r1, scm_r2;

/* Qualcomm SCM (ARMv8 SMC64): x0 fid, x1 arginfo, x2.. args; retried while TZ is interrupted. */
static uint64_t scm(uint64_t fid, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2)
{
	uint64_t r0;
	boolean_t istate = ml_set_interrupts_enabled(FALSE);
	do {
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
		r0 = x0;
		scm_r1 = x1;
		scm_r2 = x2;
	} while (r0 == 1);                      /* SCM_INTERRUPTED */
	ml_set_interrupts_enabled(istate);
	return r0;
}

static bool blob(const char *name, uint64_t *pa, uint32_t *len)
{
	DTEntry node;
	uint64_t const *v;
	unsigned int size;
	if (SecureDTLookupEntry(NULL, "/chosen/memory-map", &node) != kSuccess ||
	    SecureDTGetProperty(node, name, (void const **)&v, &size) != kSuccess || size != 16) {
		return false;
	}
	*pa = v[0];
	*len = (uint32_t)v[1];
	return true;
}

/* ---- step 1: power ---- */

static int gdsc_on(vm_offset_t r, const char *name)
{
	uint32_t was = rd(r);
	if (!(was & (1u << 31))) {
		wr(r, was & ~1u);
		for (int i = 0; i < 1000 && !(rd(r) & (1u << 31)); i++) {
			delay(10);
		}
	}
	printf("MIDO GPU: %s GDSC %08x -> %08x\n", name, was, rd(r));
	return (rd(r) & (1u << 31)) ? 0 : -1;
}

static uint32_t ldo_step(uint32_t c)
{
	wr(LDO(0x1c), c);
	dsb();
	delay(1);
	return c;
}

static int power_up(void)
{
	wr(GFX3D_CFG, 0);                                   /* gfx3d RCG: XO /1 */
	wr(GFX3D_CMD, rd(GFX3D_CMD) | 3u);                  /* update + root enable */
	for (int i = 0; i < 1000 && (rd(GFX3D_CMD) & 1u); i++) {
		delay(1);
	}
	for (uint32_t o = 0x20; o <= 0x48; o += 4) {        /* gfx branch CBCRs */
		vm_offset_t a = GCC(0x01859000UL + o);
		if (a == GX_GDSCR || a == CX_GDSCR) {
			continue;
		}
		uint32_t v = rd(a);
		if ((v & 0x80000000u) && !(v & 0x7fff0000u)) {
			wr(a, v | 1u);
		}
	}
	delay(100);
	if (gdsc_on(CX_GDSCR, "CX")) {
		return -1;
	}
	uint64_t r = scm(0x42000c02ULL, 2, 18, 0, 0);       /* restore_sec_cfg(TZ_DEVICE_GPU, 0) */
	printf("MIDO GPU: TZ restore_sec_cfg ret %llx/%llx\n", r, scm_r1);
	wr(GX_DOM_MISC, rd(GX_DOM_MISC) & ~1u);             /* OXILI_GMEM_CLAMP_IO */
	dsb();
	/* msm_gfx_ldo_init + enable_bhs_mode: the BHS is handed to the GX GDSC */
	uint32_t c = rd(LDO(0x1c));
	ldo_step((c & ~(1u << 31)) | (1u << 29));
	wr(LDO(0x00), 0x00000203);
	wr(LDO(0x04), 0x05008600);
	wr(LDO(0x08), 0x00000000);
	wr(LDO(0x0c), 0x0000c3fc);
	wr(LDO(0x14), 0x004b1102);
	dsb();
	c = rd(LDO(0x1c));
	c = ldo_step(c | (1u << 4));
	c = ldo_step(c & ~(1u << 9));
	c = ldo_step(c & ~(1u << 30));
	c = ldo_step(c & ~(1u << 1));
	c = ldo_step(c & ~(1u << 2));
	c = ldo_step(c & ~1u);
	if (gdsc_on(GX_GDSCR, "GX")) {
		return -2;
	}
	delay(100);
	if (rd(GCC(0x01859020UL)) & (1u << 31)) {
		printf("MIDO GPU: gfx3d clock still off (%08x), GPU not powered\n", rd(GCC(0x01859020UL)));
		return -3;
	}
	printf("MIDO GPU: powered, RBBM_STATUS %08x\n", gr(0x4f5));
	return 0;
}

/* ---- step 2: microcode, zap shader, CP ---- */

static uint32_t par(uint32_t v)
{
	v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}
static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return (7u << 28) | cnt | (par(cnt) << 15) | ((op & 0x7fu) << 16) | (par(op) << 23);
}
static uint32_t pkt4(uint32_t reg, uint32_t cnt)
{
	return (4u << 28) | cnt | (par(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (par(reg) << 27);
}

static void copy_words(vm_offset_t dst, vm_offset_t src, uint32_t len)
{
	for (uint32_t i = 0; i + 3 < len; i += 4) {
		wr(dst + i, *(const uint32_t *)(src + i));
	}
}

static int zap_load(void)
{
	uint64_t mdt_pa, seg_pa;
	uint32_t mdt_len, seg_len;
	if (!blob("GPU-ZAP-MDT", &mdt_pa, &mdt_len) || !blob("GPU-ZAP-SEG", &seg_pa, &seg_len) ||
	    mdt_len > 0x10000 || seg_len > 0x100000) {
		printf("MIDO GPU: no zap shader blobs\n");
		return -1;
	}
	vm_offset_t mdt = ml_static_ptovirt(mdt_pa), seg = ml_static_ptovirt(seg_pa);
	wr(APCS_VOTE, rd(APCS_VOTE) | 7u);                  /* crypto engine clocks */
	delay(100);

	/* secure SMMU page-table pool (msm_iommu_sec_pgtbl_init), once per boot */
	uint64_t r = scm(0x42000c03ULL, 1, 0, 0, 0);
	uint64_t psize = scm_r1;
	if (r || scm_r2 || !psize || psize > 0x100000) {
		printf("MIDO GPU: PTBL_SIZE ret %llx size %llx fail %llx\n", r, psize, scm_r2);
		return -2;
	}
	for (uint64_t i = 0; i < psize; i += 4) {
		wr(ptblw + i, 0);
	}
	dsb();
	r = scm(0x42000c04ULL, 0x23, P_PTBL, psize, 0);
	printf("MIDO GPU: secure PTBL %llx bytes ret %llx/%llx\n", psize, r, scm_r1);
	if (r || scm_r1) {
		return -3;
	}

	/* metadata at the region start (msm pil_boot), INIT_IMAGE, MEM_SETUP, load, AUTH */
	copy_words(zapw, mdt, mdt_len);
	dsb();
	r = scm(0x42000201ULL, 0x82, 13, P_ZAP, 0);
	if (r || scm_r1) {
		printf("MIDO GPU: zap INIT_IMAGE ret %llx/%llx\n", r, scm_r1);
		return -4;
	}
	uint32_t phoff = rd(zapw + 28), phent = rd(zapw + 40) >> 16, phnum = rd(zapw + 44) & 0xffffu;
	uint32_t lo = 0xffffffffu, hi = 0;
	for (uint32_t i = 0; i < phnum && phoff + (i + 1) * phent <= mdt_len; i++) {
		vm_offset_t ph = zapw + phoff + i * phent;
		uint32_t type = rd(ph), paddr = rd(ph + 12), memsz = rd(ph + 20), flags = rd(ph + 24);
		if (type != 1 || memsz == 0 || ((flags >> 24) & 7u) == 2u) {
			continue;
		}
		lo = paddr < lo ? paddr : lo;
		hi = paddr + memsz > hi ? paddr + memsz : hi;
	}
	if (hi <= lo) {
		printf("MIDO GPU: zap mdt has no PT_LOAD\n");
		return -5;
	}
	hi = (hi + 0xfffu) & ~0xfffu;
	r = scm(0x42000202ULL, 3, 13, P_ZAP, hi - lo);
	if (r || scm_r1) {
		printf("MIDO GPU: zap MEM_SETUP ret %llx/%llx\n", r, scm_r1);
		return -6;
	}
	for (uint32_t i = 0; i < hi - lo; i += 4) {
		wr(zapw + i, 0);
	}
	copy_words(zapw, seg, seg_len);
	dsb();
	r = scm(0x42000205ULL, 1, 13, 0, 0);
	printf("MIDO GPU: zap AUTH_AND_RESET ret %llx/%llx\n", r, scm_r1);
	return (r || scm_r1) ? -7 : 0;
}

static uint32_t g_wptr;

static int cp_start(void)
{
	uint64_t pm4_pa, pfp_pa;
	uint32_t pm4_len, pfp_len;
	if (!blob("GPU-PM4", &pm4_pa, &pm4_len) || !blob("GPU-PFP", &pfp_pa, &pfp_len) ||
	    pm4_len < 8 || pfp_len < 8 || pm4_len > 0x8000 || pfp_len > 0x8000) {
		printf("MIDO GPU: no CP microcode blobs\n");
		return -1;
	}
	for (uint32_t o = 0; o < O_PM4; o += 4) {
		wr(W(o), 0);
	}
	copy_words(W(O_PM4), ml_static_ptovirt(pm4_pa) + 4, pm4_len - 4);   /* skip the version dword */
	copy_words(W(O_PFP), ml_static_ptovirt(pfp_pa) + 4, pfp_len - 4);

	gw(0x3049, 3);                                      /* VBIF_ROUND_ROBIN_QOS_ARB */
	gw(0x0093, 1);                                      /* RBBM_AHB_CNTL0 */
	gw(0x0e96, 2);                                      /* UCHE_CACHE_WAYS */
	gw64(0x0e89, 0x0001ffffffff0000ULL);                /* UCHE_TRAP_BASE */
	gw64(0x0e87, 0x0001ffffffff0000ULL);                /* UCHE_WRITE_THRU_BASE */
	gw64(0x0e8b, 0x00100000);                           /* UCHE_GMEM_RANGE_MIN */
	gw64(0x0e8d, 0x00100000 + 0x20000 - 1);             /* UCHE_GMEM_RANGE_MAX (128 KB) */
	gw(0x046f, 0xffffffff);                             /* RBBM_PERFCTR_GPU_BUSY_MASKED */
	gw(0x0d00, (0x100u << 11) | (0x100u << 22));        /* PC_DBG_ECO_CNTL */
	gw(0x0e81, 1u << 29);                               /* UCHE_MODE_CNTL */
	gw(0x0e60, gr(0x0e60) | (1u << 10));                /* VPC_DBG_ECO_CNTL */
	gw(0x0825, 0x20);                                   /* CP_MEQ_THRESHOLDS */
	gw(0x0826, 0x20);                                   /* CP_MERCIU_SIZE */
	gw(0x0820, 0x40000030);                             /* CP_ROQ_THRESHOLDS_2 */
	gw(0x081f, 0x20100d0a);                             /* CP_ROQ_THRESHOLDS_1 */
	gw(0x0833, 0x02000000);                             /* CP_CHICKEN_DBG */
	gw(0x0094, 0xa6ffffff);                             /* RBBM_AHB_CNTL1 */
	gw(0x0095, 0x3f);                                   /* RBBM_AHB_CNTL2 */
	gw(0xf803, 0);                                      /* RBBM_SECVID_TSB_CNTL */
	gw64(0xf800, 0);                                    /* RBBM_SECVID_TSB_BASE */
	gw(0xf802, 0);                                      /* RBBM_SECVID_TSB_SIZE */
	static const uint16_t addr_mode[] = {
		0x0860, 0x0bc1, 0x0c81, 0x0cc5, 0x0d01, 0x0e05, 0x0e41, 0x0e61, 0x0e80, 0x0ec1, 0x0f01, 0xf810,
	};
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++) {
		gw(addr_mode[i], 1);                            /* every block 64-bit */
	}
	gw64(0x0838, P_WORK + O_PM4);                       /* CP_ME_INSTR_BASE */
	gw64(0x0835, P_WORK + O_PFP);                       /* CP_PFP_INSTR_BASE */
	gw64(0x0800, P_WORK + O_RING);                      /* CP_RB_BASE */
	gw(0x0802, 11u | (2u << 8) | (1u << 27));           /* CP_RB_CNTL: 16 KB, no rptr writeback */

	int zap = zap_load();                               /* before the halt release (kgsl) */
	if (zap) {
		printf("MIDO GPU: zap shader not loaded (%d): staying in secure mode, stop\n", zap);
		return -2;
	}

	uint32_t n = 0;
#define RB(v) do { wr(W(O_RING) + 4u * n, (v)); n++; } while (0)
	RB(pkt7(0x48, 8));                                  /* CP_ME_INIT */
	RB(0x2f); RB(0x3); RB(0x20000000); RB(0); RB(0); RB(0); RB(0); RB(0);
	RB(pkt7(0x66, 1)); RB(0);                           /* CP_SET_SECURE_MODE 0 */
	RB(pkt7(0x26, 0));                                  /* CP_WAIT_FOR_IDLE */
	RB(pkt7(0x3d, 3));                                  /* CP_MEM_WRITE */
	RB((uint32_t)(P_WORK + O_SCRATCH)); RB(0); RB(0xc0ffee01u);
	RB(pkt7(0x10, 0));                                  /* CP_NOP */
#undef RB
	dsb();
	gw(0x0832, 0);                                      /* CP_PFP_ME_CNTL: release the halt */
	delay(5000);
	gw(0x0807, n);                                      /* CP_RB_WPTR */
	unsigned ms;
	for (ms = 0; ms < 500 && rd(W(O_SCRATCH)) != 0xc0ffee01u; ms++) {
		delay(1000);
	}
	printf("MIDO GPU: CP %s in %u ms: RPTR %u WPTR %u HW_FAULT %08x CP_INT %08x\n",
	    rd(W(O_SCRATCH)) == 0xc0ffee01u ? "runs" : "did NOT run", ms,
	    gr(0x0806), gr(0x0807), gr(0x0b1a), gr(0x0b18));
	g_wptr = n;
	return rd(W(O_SCRATCH)) == 0xc0ffee01u ? 0 : -3;
}

/* ---- step 3: 2D blit self-test into ordinary memory ---- */

static int blit_test(void)
{
	for (uint32_t y = 0; y < 256; y++) {
		for (uint32_t x = 0; x < 256; x++) {
			wr(W(O_SRC) + y * 1024 + x * 4, (x << 16) | (y << 8) | 0xc0u);
		}
	}
	for (uint32_t i = 0; i < 256 * 256; i++) {
		wr(W(O_DST) + 4u * i, 0x5a5a5a5au);
	}
	const uint32_t fmt = 0x30;                          /* RB5_R8G8B8A8_UNORM, linear, WZYX */
	const uint32_t size = (1024 >> 6) | ((1024 * 256 >> 6) << 16);
	uint32_t k = 0;
#define IB(v) do { wr(W(O_IB) + 4u * k, (v)); k++; } while (0)
	IB(pkt4(0x0e91, 5)); IB(0); IB(0); IB(0); IB(0); IB(0x12);   /* UCHE invalidate */
	IB(pkt7(0x26, 0));
	IB(pkt7(0x6c, 5)); IB(1); IB(0); IB(0); IB(0); IB(0);        /* render mode BYPASS */
	IB(pkt7(0x1d, 1)); IB(0);                                    /* CP_SKIP_IB2_ENABLE_GLOBAL */
	IB(pkt7(0x46, 1)); IB(0x19);                                 /* PC_CCU_INVALIDATE_COLOR */
	IB(pkt4(0xe3b0, 1)); IB(0);                                  /* PC_POWER_CNTL */
	IB(pkt4(0xe4f0, 1)); IB(0);                                  /* VFD_POWER_CNTL */
	IB(pkt7(0x26, 0));
	IB(pkt4(0x0cc7, 1)); IB(0x10000000);                         /* RB_CCU_CNTL: bypass */
	IB(pkt4(0xe141, 1)); IB(0x00000008);                         /* RB_RENDER_CNTL */
	IB(pkt4(0x2100, 1)); IB(0x86000000);                         /* RB_2D_BLIT_CNTL */
	IB(pkt4(0x2180, 1)); IB(0x86000000);                         /* GRAS_2D_BLIT_CNTL */
	IB(pkt4(0x2184, 1)); IB(0x00000009);
	IB(pkt4(0xe140, 1)); IB(1u << 17);                           /* RB_CNTL: BYPASS */
	IB(pkt4(0x0cc6, 1)); IB(0x00000004);                         /* RB_MODE_CNTL */
	IB(pkt4(0x0ec2, 1)); IB(0x0000000c);                         /* SP_MODE_CNTL */
	IB(pkt4(0x0f02, 1)); IB(0x00000344);                         /* TPL1_MODE_CNTL */
	IB(pkt4(0x0e06, 1)); IB(0x00000002);                         /* HLSQ_MODE_CNTL */
	IB(pkt4(0xe000, 1)); IB(0x00000181);                         /* GRAS_CL_CNTL */
	IB(pkt7(0x6c, 1)); IB(5);                                    /* BLIT2D */
	IB(pkt4(0x2107, 9)); IB(fmt); IB((uint32_t)(P_WORK + O_SRC)); IB(0); IB(size);
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(0x2181, 1)); IB(fmt);
	IB(pkt4(0x2110, 9)); IB(fmt); IB((uint32_t)(P_WORK + O_DST)); IB(0); IB(size);
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(0x2182, 1)); IB(fmt);
	IB(pkt7(0x2c, 5)); IB(1); IB(0); IB(255 | (255u << 16)); IB(0); IB(255 | (255u << 16));
	IB(pkt7(0x6c, 1)); IB(8);                                    /* END2D */
	IB(pkt7(0x26, 0));
	IB(pkt7(0x46, 4)); IB(0x1d); IB((uint32_t)(P_WORK + O_SCRATCH + 0x100)); IB(0); IB(1);
	IB(pkt7(0x46, 4)); IB(0x04); IB((uint32_t)(P_WORK + O_SCRATCH + 0x200)); IB(0); IB(2);
	IB(pkt7(0x26, 0));
#undef IB
	uint32_t n = g_wptr;
#define RB(v) do { wr(W(O_RING) + 4u * n, (v)); n++; } while (0)
	RB(pkt7(0x3f, 3)); RB((uint32_t)(P_WORK + O_IB)); RB(0); RB(k);   /* CP_INDIRECT_BUFFER */
	RB(pkt7(0x26, 0));
	RB(pkt7(0x3d, 3)); RB((uint32_t)(P_WORK + O_SCRATCH)); RB(0); RB(0xc0ffee02u);
#undef RB
	dsb();
	gw(0x0807, n);
	unsigned ms;
	for (ms = 0; ms < 2000 && rd(W(O_SCRATCH)) != 0xc0ffee02u; ms++) {
		delay(1000);
	}
	uint32_t same = 0;
	for (uint32_t i = 0; i < 256 * 256; i++) {
		same += rd(W(O_DST) + 4u * i) == rd(W(O_SRC) + 4u * i);
	}
	printf("MIDO GPU: 2D blit %s in %u ms: %u of 65536 pixels match\n",
	    ms < 2000 ? "done" : "TIMED OUT", ms, same);
	g_wptr = n;
	return same == 65536 ? 0 : -1;
}

static void gpu_thread(void *arg, wait_result_t wr_)
{
	(void)arg;
	(void)wr_;
	delay(20 * 1000 * 1000);                            /* let the boot log settle first */
	printf("MIDO GPU: bring-up starting\n");
	gcc = ml_io_map(P_GCC, 0x20000);
	gpu = ml_io_map(P_GPU, 0x40000);
	smmu = ml_io_map(P_GPU + 0x40000, 0x10000);
	ptblw = ml_io_map(P_PTBL, 0x100000);
	zapw = ml_io_map(P_ZAP, 0x10000);
	work = ml_io_map(P_WORK, WORK_SIZE);
	if (!gcc || !gpu || !smmu || !ptblw || !zapw || !work) {
		printf("MIDO GPU: ml_io_map failed\n");
		return;
	}
	if (power_up() == 0 && cp_start() == 0 && blit_test() == 0) {
		printf("MIDO GPU: READY (power, zap shader, CP, 2D blit all OK)\n");
	} else {
		printf("MIDO GPU: bring-up stopped\n");
	}
}

void mido_gpu_init(void);
void
mido_gpu_init(void)
{
	uint32_t on = 0;
	if (!PE_parse_boot_argn("midogpu", &on, sizeof(on)) || !on) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start_priority(gpu_thread, NULL, 80, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
