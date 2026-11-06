#!/usr/bin/env python3
"""Apply the MIDO (Redmi Note 4, MSM8953) changes to an xnu-8796 tree that
already has ios-sweet patches 0001-0081 applied.

  mido_xnu.py <xnu source root>

Every edit is an exact-string replacement with an expected match count, so a
changed upstream fails loudly instead of producing a wrong patch. The result
is diffed with git into xnu/patches/0100-mido-board.patch (see mkpatch.sh).

MIDO stays MACHINE_CONFIG=SWEET (keeps upstream staging + drivers) and adds a
`MIDO` switch in SWEET.h:
  - 4 KB pages: Cortex-A53 has no 16K translation granule (ID_AA64MMFR0.TGran16=0)
  - GICv2 (qcom,msm-qgic2: GICD 0x0B000000, GICC 0x0B002000) instead of GICv3
  - virtual timer PPI = INTID 20 (DT <1 4 ...>, measured: 29 IRQs / 300 ms)
  - ARMv8.0 code generation (no LSE/RDM/FP16 on A53)
  - framebuffer 0x90001000, 1080x1920, 32 bpp, stride 4320 (shim switches LK's pipe to 32 bpp)
"""
import sys
from pathlib import Path

root = Path(sys.argv[1])
failures = []


def edit(rel, old, new, count=1):
    p = root / rel
    text = p.read_text()
    n = text.count(old)
    if n != count:
        failures.append(f'{rel}: expected {count} match(es), found {n}: {old[:70]!r}')
        return
    p.write_text(text.replace(old, new))


# ---------------------------------------------------------------- SWEET.h
edit('pexpert/pexpert/arm64/SWEET.h',
     '#define SWEET                     1\n',
     '#define SWEET                     1\n'
     '\n'
     '/*\n'
     ' * MIDO: Redmi Note 4 (Qualcomm MSM8953 / Snapdragon 625), 8x Cortex-A53\n'
     ' * (ARMv8.0: no LSE, no 16K granule), GICv2, eMMC, LK bootloader. Builds on\n'
     ' * the SWEET machine config; MIDO selects the differing hardware paths.\n'
     ' */\n'
     '#define MIDO                      1\n')
edit('pexpert/pexpert/arm64/SWEET.h',
     '#define __ARM_16K_PG__            1\n',
     '#if !defined(MIDO)\n'
     '#define __ARM_16K_PG__            1   /* MIDO: A53 has no 16K granule -> 4K pages */\n'
     '#endif\n')
# Build #4: "Invalid register name pan" (copyio.c, pcb.c, platform_tests.c).
# PAN is ARMv8.1; Cortex-A53 has none, so MIDO runs without it.
edit('pexpert/pexpert/arm64/SWEET.h',
     '#define __ARM_PAN_AVAILABLE__     1\n',
     '#if !defined(MIDO)\n'
     '#define __ARM_PAN_AVAILABLE__     1   /* MIDO: A53 is ARMv8.0, no PAN */\n'
     '#endif\n')
edit('pexpert/pexpert/arm64/SWEET.h',
     '#define HAS_GIC_V3                1\n',
     '#if defined(MIDO)\n'
     '#define HAS_GIC_V2                1\n'
     '#else\n'
     '#define HAS_GIC_V3                1\n'
     '#endif\n')
edit('pexpert/pexpert/arm64/SWEET.h',
     '#define SWEET_VTIMER_INTID        19\n',
     '#if defined(MIDO)\n'
     '#define SWEET_VTIMER_INTID        20      /* MSM8953 DT: <GIC_PPI 4 ...> */\n'
     '#else\n'
     '#define SWEET_VTIMER_INTID        19\n'
     '#endif\n')
edit('pexpert/pexpert/arm64/SWEET.h',
     '/* End of GICv3 register definitions */\n',
     '/* End of GICv3 register definitions */\n'
     '\n'
     '/* GICv2 (MIDO): distributor + memory-mapped CPU interface, Arm IHI 0048B */\n'
     '#define GICD2_CTLR                0x000\n'
     '#define GICD2_ISENABLER0          0x100\n'
     '#define GICD2_IPRIORITYR0         0x400\n'
     '#define GICD2_ITARGETSR0          0x800\n'
     '#define GICD2_SGIR                0xF00\n'
     '#define GICC2_CTLR                0x00\n'
     '#define GICC2_PMR                 0x04\n'
     '#define GICC2_BPR                 0x08\n'
     '#define GICC2_IAR                 0x0C\n'
     '#define GICC2_EOIR                0x10\n'
     '\n'
     '/* MIDO framebuffer (LK splash carve-out, switched to 32 bpp by the shim) */\n'
     '#define MIDO_FB_PHYS              0x90001000ULL\n'
     '#define MIDO_FB_BLOCK_PHYS        0x90000000ULL   /* 2MB-aligned base containing it */\n'
     '#define MIDO_FB_HEIGHT            1920u\n'
     '#define MIDO_FB_STRIDE            4320u\n')

# ---------------------------------------------------------------- MakeInc.def
edit('makedefs/MakeInc.def',
     'MACHINE_FLAGS_ARM64_SWEET = -DARM64_BOARD_CONFIG_SWEET                -march=armv8.2-a+crypto\n',
     '# MIDO (Cortex-A53): ARMv8.0 only, so the kernel never emits LSE/RDM/FP16.\n'
     '# -mcpu, not -march: `-arch arm64 -mmacosx-version-min` makes clang default to an\n'
     '# Apple CPU whose features (LSE, RCpc, FP16, ...) -march does not remove. Build #3\n'
     '# panicked on `ldadd` in lck_grp_reference (843 LSE sites in 517 functions).\n'
     '# MIDO_4K_VM: iOS user VA layout with 4K pages (see osfmk/mach/arm/vm_param.h).\n'
     '# MIDO_CPU_ARMV8_0: for files that do not include the board header (commpage_asm.s).\n'
     'MACHINE_FLAGS_ARM64_SWEET = -DARM64_BOARD_CONFIG_SWEET -DMIDO_4K_VM -DMIDO_CPU_ARMV8_0   -mcpu=cortex-a53+crc+crypto\n')

# First boot on the phone (build #2): XNU drew its stage-0 band, then hung
# silently at the first kernel-VA access after the MMU came on. With 4K pages
# T1SZ_BOOT is 25 (39-bit TTBR1: only 0xffffff80_0000_0000 and up translate),
# but ARM_LARGE_MEMORY (forced for every MacOSX-platform build) links the
# kernel at 0xfffffe00_0700_4000 (-2 TB), which TTBR1 cannot reach. Use the
# classic iOS layout (link 0xfffffff0_0700_4000) for the SWEET/MIDO config.
edit('makedefs/MakeInc.def',
     '# Use ARM_LARGE_MEMORY config for all MacOSX targets.\n'
     'ifneq ($(filter $(PLATFORM),MacOSX),)\n'
     'LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1\n'
     'ARM64_PLKSEG_ADDR  =0xfffffe0004004000\n'
     'ARM64_LINK_ADDR    =0xfffffe0007004000\n'
     'endif\n',
     '# Use ARM_LARGE_MEMORY config for all MacOSX targets.\n'
     '# MIDO (4K pages, 39-bit kernel VA) cannot map the -2 TB layout: keep the iOS one.\n'
     'ifneq ($(filter $(PLATFORM),MacOSX),)\n'
     'ifneq ($(CURRENT_MACHINE_CONFIG),SWEET)\n'
     'LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1\n'
     'ARM64_PLKSEG_ADDR  =0xfffffe0004004000\n'
     'ARM64_LINK_ADDR    =0xfffffe0007004000\n'
     'endif\n'
     'endif\n')

# ---------------------------------------------------------------- vm_param.h
# CI run 42149040200: pmap.c static_assert (4K only) requires the commpage
# (0xFFFFFC000) to be nested above MACH_VM_MAX_ADDRESS at L1 (1 GB) granularity.
# The macOS-platform value (128 TB) fails; the iOS value 0xFC0000000 is exactly
# that 1 GB boundary, and the userland is iOS anyway.
edit('osfmk/mach/arm/vm_param.h',
     '#if defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)\n'
     '#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL\n',
     '#if (defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)) && !defined(MIDO_4K_VM)\n'
     '#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL\n')

# ---------------------------------------------------------------- protos.h
edit('pexpert/pexpert/arm/protos.h',
     '#if defined(ARM64_BOARD_CONFIG_SWEET)\nextern uint32_t sweet_vtimer_intid;\n#endif\n',
     '#if defined(ARM64_BOARD_CONFIG_SWEET)\nextern uint32_t sweet_vtimer_intid;\n'
     '/* MIDO GICv2 helpers (pexpert/arm/pe_fiq.c) */\n'
     'uint32_t mido_gic_ack(void);\n'
     'void mido_gic_eoi(uint32_t iar);\n'
     'void mido_gic_send_sgi(unsigned int cpu, unsigned int sgi);\n'
     '#endif\n')

# ---------------------------------------------------------------- pe_fiq.c (GICv2)
edit('pexpert/arm/pe_fiq.c',
     '#else\nvoid\npe_init_fiq()\n{\n}\n#endif\n',
     r'''#elif HAS_GIC_V2
/*
 * MIDO: GICv2 with security extensions (qcom,msm-qgic2). TrustZone owns
 * Group 0; we run Non-secure EL1, so all our interrupts (vtimer PPI, SGI
 * IPIs) are Group 1 and arrive as IRQs (see sleh_irq). Measured in the mido
 * shim: GICD_TYPER 0x4EA, vtimer INTID 20 delivers ~100 Hz from NS-EL1.
 */
SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;

static vm_offset_t gicd_base;
static vm_offset_t gicc_base;
/* GICv2 SGIs target CPU-interface bits, not affinities: each CPU records its
 * own bit (banked GICD_ITARGETSR0) when it initializes its interface. */
static volatile uint8_t gic_iface_mask[MAX_CPUS];

#define gicd2_read32(o)     (*(volatile uint32_t *)(gicd_base + (o)))
#define gicd2_write32(o, v) (*(volatile uint32_t *)(gicd_base + (o)) = (v))
#define gicc2_read32(o)     (*(volatile uint32_t *)(gicc_base + (o)))
#define gicc2_write32(o, v) (*(volatile uint32_t *)(gicc_base + (o)) = (v))

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	if (!gicd_base) {
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess || prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: GICv2 node needs reg = <gicd base size gicc base size>", __func__);
		}
		void const *timer_prop;
		unsigned int timer_size;
		if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess &&
		    timer_size == sizeof(uint32_t)) {
			uint32_t intid = *(const uint32_t *)timer_prop;
			if (intid < 16 || intid >= 32) {
				panic("%s: virtual timer INTID %u is not a PPI", __func__, intid);
			}
			sweet_vtimer_intid = intid;
		}
		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();
		uint64_t const *r = (uint64_t const *)prop;
		gicd_base = ml_io_map(soc_base_phys + r[0], r[1]);
		gicc_base = ml_io_map(soc_base_phys + r[2], r[3]);
		if (!gicd_base || !gicc_base) {
			panic("%s: cannot map GICv2 regions", __func__);
		}
		kprintf("MIDO GIC: GICv2 TYPER=0x%x vtimer INTID %u\n",
		    gicd2_read32(0x004), sweet_vtimer_intid);
		/* Distributor: enable Group 1 forwarding (NS view: bit 0). */
		gicd2_write32(GICD2_CTLR, gicd2_read32(GICD2_CTLR) | 0x1);
	}

	unsigned int cpu = ml_get_cpu_number_local();
	uint8_t self = (uint8_t)(gicd2_read32(GICD2_ITARGETSR0) & 0xff);
	if (cpu < MAX_CPUS) {
		gic_iface_mask[cpu] = self;
	}
	/* Banked SGI/PPI priorities and enables for this CPU. */
	for (unsigned int i = 0; i < 32; i += 4) {
		gicd2_write32(GICD2_IPRIORITYR0 + i, 0xA0A0A0A0);
	}
	gicd2_write32(GICD2_ISENABLER0, (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
	gicc2_write32(GICC2_PMR, 0xF0);
	gicc2_write32(GICC2_BPR, 0);
	gicc2_write32(GICC2_CTLR, gicc2_read32(GICC2_CTLR) | 0x1);   /* EnableGrp1, EOImode 0 */
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_isb(ISB_SY);
}

uint32_t
mido_gic_ack(void)
{
	return gicc2_read32(GICC2_IAR);
}

void
mido_gic_eoi(uint32_t iar)
{
	gicc2_write32(GICC2_EOIR, iar);
}

void
mido_gic_send_sgi(unsigned int cpu, unsigned int sgi)
{
	uint32_t mask = cpu < MAX_CPUS ? gic_iface_mask[cpu] : 0;
	if (!mask) {
		/* Target not initialized yet: MSM8953 interfaces follow MPIDR (Aff1*4 + Aff0). */
		const ml_topology_info_t *t = ml_get_topology_info();
		if (cpu < t->num_cpus) {
			uint32_t id = t->cpus[cpu].phys_id;
			mask = 1U << (((id >> 8) & 0xff) * 4 + (id & 0xff));
		}
	}
	__builtin_arm_dsb(DSB_ISHST);
	gicd2_write32(GICD2_SGIR, ((mask & 0xff) << 16) | (sgi & 0xf));
	__builtin_arm_isb(ISB_SY);
}
#else
void
pe_init_fiq()
{
}
#endif
''')

# ---------------------------------------------------------------- sleh.c
edit('osfmk/arm64/sleh.c',
     '\tuint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");\n'
     '\tuint32_t intid = (uint32_t)(iar & 0xFFFFFF);\n',
     '#if defined(MIDO)\n'
     '\t/* GICv2: memory-mapped CPU interface */\n'
     '#define SWEET_GIC_EOI(v) mido_gic_eoi((uint32_t)(v))\n'
     '\tuint64_t iar = mido_gic_ack();\n'
     '\tuint32_t intid = (uint32_t)(iar & 0x3FF);\n'
     '#else\n'
     '#define SWEET_GIC_EOI(v) __builtin_arm_wsr64("ICC_EOIR1_EL1", (v))\n'
     '\tuint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");\n'
     '\tuint32_t intid = (uint32_t)(iar & 0xFFFFFF);\n'
     '#endif\n')
edit('osfmk/arm64/sleh.c',
     '__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);',
     'SWEET_GIC_EOI(iar);', count=3)

# ---------------------------------------------------------------- SweetCPU.cpp
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'SweetCPU::signalCPU(IOCPU *target)\n{\n',
     'SweetCPU::signalCPU(IOCPU *target)\n{\n'
     '#if defined(MIDO)\n'
     '\t/* GICv2: GICD_SGIR with a CPU-interface target list */\n'
     '\tmido_gic_send_sgi(target->getCPUNumber(), SWEET_IPI_SGI);\n'
     '\treturn;\n'
     '#endif\n')
# C++ needs a C-linkage prototype for the pe_fiq.c helper.
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'void\nSweetCPU::signalCPU(IOCPU *target)\n',
     '#if defined(MIDO)\nextern "C" void mido_gic_send_sgi(unsigned int cpu, unsigned int sgi);\n#endif\n\n'
     'void\nSweetCPU::signalCPU(IOCPU *target)\n')

# ---------------------------------------------------------------- start.s
edit('osfmk/arm64/start.s',
     '\tMOV64\tx0, (0x9c000000 + 2368 * 4320)\n',
     '#if defined(MIDO)\n'
     '\tMOV64\tx0, (0x90001000 + 1888 * 4320)\t\t/* mido fb, 32 rows above the bottom */\n'
     '#else\n'
     '\tMOV64\tx0, (0x9c000000 + 2368 * 4320)\n'
     '#endif\n')
edit('osfmk/arm64/start.s',
     '\tMOV64\tx10, 0x9c000000\n'
     '\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n'
     '\torr\t\tx11, x11, x10\n'
     '\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)\n'
     '\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)\n'
     '\tadd\t\tx13, x25, #(2 * PGBYTES)\n'
     '\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]\n',
     '#if defined(MIDO)\n'
     '\t/*\n'
     '\t * MIDO: 4K granule -> 2MB L2 blocks. The 32 bpp framebuffer\n'
     '\t * (0x90001000, 8.3MB) needs four blocks from 0x90000000. It shares\n'
     '\t * L1 index 2 (0x80000000-0xBFFFFFFF) with the first RAM page\n'
     '\t * (0xA0000000), i.e. the first V=P L2 page.\n'
     '\t */\n'
     '\tMOV64\tx10, 0x90000000\n'
     '\tmov\t\tx9, #4\n'
     'Lmido_fb_block:\n'
     '\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n'
     '\torr\t\tx11, x11, x10\n'
     '\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)\n'
     '\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)\n'
     '\tadd\t\tx13, x25, #(2 * PGBYTES)\n'
     '\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]\n'
     '\tadd\t\tx10, x10, #(ARM_TT_L2_SIZE)\n'
     '\tsubs\tx9, x9, #1\n'
     '\tb.ne\tLmido_fb_block\n'
     '#else\n'
     '\tMOV64\tx10, 0x9c000000\n'
     '\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n'
     '\torr\t\tx11, x11, x10\n'
     '\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)\n'
     '\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)\n'
     '\tadd\t\tx13, x25, #(2 * PGBYTES)\n'
     '\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]\n'
     '#endif\n')

# ---------------------------------------------------------------- start.s progress marks
# MIDO_MARK n, color: a 48x32 square right of the stage-0 band (row 1888),
# square n at x = 64 + 56*(n-1). No stack yet, so x0-x3 are parked in a C
# global (sweet_early.c); x16/x17 are clobbered (IP0/IP1 scratch). Works MMU
# off (PA) and MMU on (V=P map, fb blocks mapped above). Last square = last
# step reached.
edit('osfmk/arm64/start.s',
     '.macro MSR_TCR_EL1_X1\n',
     '#if defined(MIDO)\n'
     '.macro MIDO_MARK n, color\n'
     '\tadrp\tx16, EXT(mido_mark_save)@page\n'
     '\tadd\t\tx16, x16, EXT(mido_mark_save)@pageoff\n'
     '\tstp\t\tx0, x1, [x16]\n'
     '\tstp\t\tx2, x3, [x16, #16]\n'
     '\tMOV64\tx0, (0x90001000 + 1888 * 4320 + (64 + 56 * (\\n - 1)) * 4)\n'
     '\tMOV64\tx1, \\color\n'
     '\tmov\t\tx2, #32\n'
     'Lmido_mark_row\\@:\n'
     '\tmov\t\tx3, #48\n'
     '\tmov\t\tx17, x0\n'
     'Lmido_mark_px\\@:\n'
     '\tstr\t\tw1, [x17], #4\n'
     '\tsubs\tx3, x3, #1\n'
     '\tb.ne\tLmido_mark_px\\@\n'
     '\tadd\t\tx0, x0, #4096\n'
     '\tadd\t\tx0, x0, #224\t\t\t\t\t\t// stride 4320 (not an add immediate)\n'
     '\tsubs\tx2, x2, #1\n'
     '\tb.ne\tLmido_mark_row\\@\n'
     '\tdsb\t\tsy\n'
     '\tldp\t\tx0, x1, [x16]\n'
     '\tldp\t\tx2, x3, [x16, #16]\n'
     '.endmacro\n'
     '#else\n'
     '.macro MIDO_MARK n, color\n'
     '.endmacro\n'
     '#endif\n'
     '\n'
     '.macro MSR_TCR_EL1_X1\n')
# Anchors are specific to start_first_cpu: start_cpu (secondary CPUs) has its
# own LowExceptionVectorBase load and `b common_start` (found by the count check).
edit('osfmk/arm64/start.s',
     '\t// Set low reset vector before attempting any loads\n'
     '\tadrp\tx0, EXT(LowExceptionVectorBase)@page\n'
     '\tadd\t\tx0, x0, EXT(LowExceptionVectorBase)@pageoff\n\tMSR_VBAR_EL1_X0\n',
     '\t// Set low reset vector before attempting any loads\n'
     '\tadrp\tx0, EXT(LowExceptionVectorBase)@page\n'
     '\tadd\t\tx0, x0, EXT(LowExceptionVectorBase)@pageoff\n\tMSR_VBAR_EL1_X0\n'
     '\tMIDO_MARK 1, 0xffff0000\t\t\t\t\t// red: low vectors set\n')
edit('osfmk/arm64/start.s',
     '\t/* Ensure TTEs are visible */\n\tdsb\t\tish\n\n\n\tb\t\tcommon_start\n',
     '\t/* Ensure TTEs are visible */\n\tdsb\t\tish\n\n\n'
     '\tMIDO_MARK 2, 0xffff8000\t\t\t\t\t// orange: bootstrap page tables built\n'
     '\tb\t\tcommon_start\n')
edit('osfmk/arm64/start.s',
     '\tmsr\t\tMAIR_EL1, x0\n\tisb\n\ttlbi\tvmalle1\n\tdsb\t\tish\n',
     '\tmsr\t\tMAIR_EL1, x0\n\tisb\n\ttlbi\tvmalle1\n\tdsb\t\tish\n'
     '\tMIDO_MARK 3, 0xffffff00\t\t\t\t\t// yellow: TCR/TTBR/MAIR set\n')
edit('osfmk/arm64/start.s',
     '\t// Enable caches, MMU, ROP and JOP\n',
     '\tMIDO_MARK 4, 0xff00ff00\t\t\t\t\t// green: about to enable the MMU\n'
     '\t// Enable caches, MMU, ROP and JOP\n')
edit('osfmk/arm64/start.s',
     '\tMOV64   x0, SCTLR_EL1_DEFAULT\n\tMSR_SCTLR_EL1_X0\n\tisb\t\tsy\n',
     '\tMOV64   x0, SCTLR_EL1_DEFAULT\n\tMSR_SCTLR_EL1_X0\n\tisb\t\tsy\n'
     '\tMIDO_MARK 5, 0xff00ffff\t\t\t\t\t// cyan: running with the MMU on\n')
edit('osfmk/arm64/start.s',
     '\t// Return to arm_init()\n\tret\n',
     '\tMIDO_MARK 6, 0xff0040ff\t\t\t\t\t// blue: returning to arm_init (C)\n'
     '\t// Return to arm_init()\n\tret\n')

# ---------------------------------------------------------------- commpage_asm.s
# Build #4: "instruction requires: lse" for the two PFZ trylock routines in the
# commpage (mapped into every process). casa wN, wM, [xL] with expected 0 and
# new 1 becomes an LDAXR/STXR loop; only caller-saved x9-x15 are used, w10
# returns the old value, acquire pairs with the existing stlr release.
def pfz_casa(lock, tag):
    # commpage_asm.s does not include SWEET.h (build #5 took the #else branch),
    # so test the command-line flag instead of MIDO.
    return ('#if defined(MIDO_CPU_ARMV8_0)\n'
            f'Lmido_{tag}_retry:\t\t\t\t\t\t// ARMv8.0 casa w10, w11, [{lock}]\n'
            f'\tldaxr\tw12, [{lock}]\n'
            f'\tcbnz\tw12, Lmido_{tag}_held\n'
            f'\tstxr\tw13, w11, [{lock}]\n'
            f'\tcbnz\tw13, Lmido_{tag}_retry\n'
            f'\tmov\t\tw10, w12\n'
            f'\tb\t\tLmido_{tag}_done\n'
            f'Lmido_{tag}_held:\n'
            '\tclrex\n'
            '\tmov\t\tw10, w12\n'
            f'Lmido_{tag}_done:\n'
            '#else\n'
            f'\tcasa\tw10, w11, [{lock}]\t // Atomic CAS with acquire barrier\n'
            '#endif\n')


edit('osfmk/arm/commpage/commpage_asm.s',
     '\t// Try to grab the lock\n\tcasa\tw10, w11, [x3]\t // Atomic CAS with acquire barrier\n',
     '\t// Try to grab the lock\n' + pfz_casa('x3', 'enq'))
edit('osfmk/arm/commpage/commpage_asm.s',
     '\tcasa\tw10, w11, [x2]\t // Atomic CAS with acquire barrier\n',
     pfz_casa('x2', 'deq'))

# ---------------------------------------------------------------- sweet_early.c
edit('osfmk/arm64/sweet_early.c',
     'static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)0x9c000000;\n',
     '#if defined(MIDO)\n'
     'static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)MIDO_FB_PHYS;\n'
     '/* start.s MIDO_MARK parks x0-x3 here (no stack that early). */\n'
     'extern uint64_t mido_mark_save[4];\n'
     'uint64_t mido_mark_save[4];\n'
     '#else\n'
     'static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)0x9c000000;\n'
     '#endif\n')
edit('osfmk/arm64/sweet_early.c',
     '#define EARLY_HEIGHT 2400u\n',
     '#if defined(MIDO)\n#define EARLY_HEIGHT MIDO_FB_HEIGHT\n#else\n#define EARLY_HEIGHT 2400u\n#endif\n')

# ---------------------------------------------------------------- arm_vm_init.c
edit('osfmk/arm64/arm_vm_init.c',
     'static void\nsweet_early_map_final(void)\n{\n',
     '#if defined(MIDO)\n'
     '/*\n'
     ' * 4K granule: an L2 block is 2MB, so the 8.3MB 32 bpp framebuffer needs\n'
     ' * four consecutive blocks. Never panic here: if the slots are not free,\n'
     ' * return 0 and the caller drops the early console instead.\n'
     ' */\n'
     'static int\n'
     'mido_early_map_final(void)\n'
     '{\n'
     '\tconst vm_map_address_t va = SWEET_EARLY_FB_VA;\n'
     '\tif (va + 4 * ARM_TT_L2_SIZE > gVirtBase) {\n'
     '\t\treturn 0;\n'
     '\t}\n'
     '\ttt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];\n'
     '\tif ((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) != (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) {\n'
     '\t\treturn 0;\n'
     '\t}\n'
     '\ttt_entry_t *l2 = (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK);\n'
     '\tunsigned int idx = (unsigned int)((va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT);\n'
     '\tfor (unsigned int i = 0; i < 4; i++) {\n'
     '\t\tif (idx + i >= TTE_PGENTRIES || l2[idx + i] != ARM_TTE_EMPTY) {\n'
     '\t\t\treturn 0;\n'
     '\t\t}\n'
     '\t}\n'
     '\tfor (unsigned int i = 0; i < 4; i++) {\n'
     '\t\tl2[idx + i] = (MIDO_FB_BLOCK_PHYS + i * ARM_TT_L2_SIZE) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |\n'
     '\t\t    ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) |\n'
     '\t\t    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) |\n'
     '\t\t    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX;\n'
     '\t}\n'
     '\t__asm__ volatile("dsb ishst" ::: "memory");\n'
     '\treturn 1;\n'
     '}\n'
     '#endif\n'
     '\n'
     'static void __unused\nsweet_early_map_final(void)\n{\n')
edit('osfmk/arm64/arm_vm_init.c',
     '\t/* phystokv() must still refer to the bootstrap aperture here. */\n'
     '\tsweet_early_map_final();\n',
     '\t/* phystokv() must still refer to the bootstrap aperture here. */\n'
     '#if defined(MIDO)\n'
     '\tint mido_fb_alias = mido_early_map_final();\n'
     '#else\n'
     '\tsweet_early_map_final();\n'
     '#endif\n')
edit('osfmk/arm64/arm_vm_init.c',
     '\tsweet_early_relocate(SWEET_EARLY_FB_VA);\n',
     '#if defined(MIDO)\n'
     '\tif (mido_fb_alias) {\n'
     '\t\tsweet_early_relocate(SWEET_EARLY_FB_VA + (MIDO_FB_PHYS - MIDO_FB_BLOCK_PHYS));\n'
     '\t} else {\n'
     '\t\tsweet_early_disable();   /* no alias: keep running without the early console */\n'
     '\t}\n'
     '#else\n'
     '\tsweet_early_relocate(SWEET_EARLY_FB_VA);\n'
     '#endif\n')

# zalloc: the macOS zone map is 128 GB of VA. With 4K pages and no
# ARM_LARGE_MEMORY the whole kernel map is 0xffffffe000000000..0xfffffffbffffffff
# (112 GB), so kmem_range_init panicked "vm_map_locate_space failing for claim
# zones" (boot of build #7). Use the iOS sizes; mido has 2 GB of RAM.
edit('osfmk/kern/zalloc.c',
     '#if XNU_PLATFORM_MacOSX\n'
     '#define ZONE_MAP_MAX            (32ULL << 30)\n',
     '#if XNU_PLATFORM_MacOSX && !defined(MIDO_4K_VM)\n'
     '#define ZONE_MAP_MAX            (32ULL << 30)\n')

# vm_kern: build #8 (24 GB zones) still panicked "vm_map_locate_space failing for
# claim zones". On failure, dump the kernel map, every claim and every big hole to the
# console, then retry once from the bottom of the map before panicking.
edit('osfmk/vm/vm_kern.c',
     '__startup_func\n'
     'static void\n'
     'kmem_scramble_ranges(void)\n'
     '{\n',
     '#if defined(MIDO_4K_VM)\n'
     '__startup_func\n'
     'static void\n'
     'mido_kmem_claims_dump(uint32_t fail_idx, vm_map_offset_t cursor)\n'
     '{\n'
     '\tvm_map_size_t lf = 0;\n'
     '\tvm_map_entry_t e;\n'
     '\tvm_map_offset_t prev;\n'
     '\n'
     '\tvm_map_sizes(kernel_map, NULL, NULL, &lf);\n'
     '\tprintf("MIDO kmap %llx-%llx cur %llx lfree %lluM\\n",\n'
     '\t    (uint64_t)kernel_map->min_offset, (uint64_t)kernel_map->max_offset,\n'
     '\t    (uint64_t)cursor, (uint64_t)(lf >> 20));\n'
     '\tfor (uint32_t k = 0; k < kmem_claim_count; k++) {\n'
     '\t\tprintf("%c%s %lluM%s", k == fail_idx ? \'*\' : \' \',\n'
     '\t\t    kmem_claims[k].kc_name,\n'
     '\t\t    (unsigned long long)(kmem_claims[k].kc_size >> 20),\n'
     '\t\t    (k & 1) ? "\\n" : " |");\n'
     '\t}\n'
     '\tprintf("\\n");\n'
     '\tprev = kernel_map->min_offset;\n'
     '\tfor (e = vm_map_first_entry(kernel_map); e != vm_map_to_entry(kernel_map);\n'
     '\t    e = e->vme_next) {\n'
     '\t\tif (e->vme_start - prev >= (16ULL << 20)) {\n'
     '\t\t\tprintf("hole %llx-%llx %lluM\\n", (uint64_t)prev,\n'
     '\t\t\t    (uint64_t)e->vme_start, (uint64_t)((e->vme_start - prev) >> 20));\n'
     '\t\t}\n'
     '\t\tprev = e->vme_end;\n'
     '\t}\n'
     '\tif (kernel_map->max_offset - prev >= (16ULL << 20)) {\n'
     '\t\tprintf("hole %llx-%llx (tail)\\n", (uint64_t)prev,\n'
     '\t\t    (uint64_t)kernel_map->max_offset);\n'
     '\t}\n'
     '}\n'
     '#endif\n'
     '\n'
     '__startup_func\n'
     'static void\n'
     'kmem_scramble_ranges(void)\n'
     '{\n')
edit('osfmk/vm/vm_kern.c',
     '\t\tif (vm_map_locate_space(kernel_map, sp.kc_size, 0,\n'
     '\t\t    VM_MAP_KERNEL_FLAGS_ANYWHERE(), &start, NULL) != KERN_SUCCESS) {\n'
     '\t\t\tpanic("kmem_range_init: vm_map_locate_space failing for claim %s",\n'
     '\t\t\t    sp.kc_name);\n'
     '\t\t}\n',
     '\t\tif (vm_map_locate_space(kernel_map, sp.kc_size, 0,\n'
     '\t\t    VM_MAP_KERNEL_FLAGS_ANYWHERE(), &start, NULL) != KERN_SUCCESS) {\n'
     '#if defined(MIDO_4K_VM)\n'
     '\t\t\tmido_kmem_claims_dump(i, start);\n'
     '\t\t\tstart = kernel_map->min_offset;\n'
     '\t\t\tif (vm_map_locate_space(kernel_map, sp.kc_size, 0,\n'
     '\t\t\t    VM_MAP_KERNEL_FLAGS_ANYWHERE(), &start, NULL) == KERN_SUCCESS) {\n'
     '\t\t\t\tprintf("MIDO: claim %s placed on retry at %llx\\n",\n'
     '\t\t\t\t    sp.kc_name, (uint64_t)start);\n'
     '\t\t\t} else\n'
     '#endif\n'
     '\t\t\tpanic("kmem_range_init: vm_map_locate_space failing for claim %s",\n'
     '\t\t\t    sp.kc_name);\n'
     '\t\t}\n')

if failures:
    print('MIDO EDIT FAILURES:')
    for f in failures:
        print('  ' + f)
    sys.exit(1)
print('mido_xnu.py: all edits applied')
