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


def create(rel, text):
    p = root / rel
    if p.exists():
        failures.append(f'{rel}: already exists')
        return
    p.write_text(text)


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
     'MACHINE_FLAGS_ARM64_SWEET = -DARM64_BOARD_CONFIG_SWEET -DMIDO_4K_VM -DMIDO_CPU_ARMV8_0 -DMIDO_BOARD -mcpu=cortex-a53+crc+crypto\n')

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

# vm_compressor: the build #9 dump showed the largest kernel-map hole is 46621M,
# but zones (24576M) + compressor (24562M, the macOS pool of 16 x 1.5 GB RAM) already
# exceed it, so kmem_add_extra_claims underflowed (kmem_ptr_range 128 TB, data 16 EB).
# Cap the pool at 4 x RAM (6 GB) unless the vm_compression_limit boot-arg is given.
edit('osfmk/vm/vm_compressor.c',
     '\tif (compressor_pool_size > VM_COMPRESSOR_MAX_POOL_SIZE) {\n'
     '\t\tcompressor_pool_size = VM_COMPRESSOR_MAX_POOL_SIZE;\n'
     '\t}\n',
     '\tif (compressor_pool_size > VM_COMPRESSOR_MAX_POOL_SIZE) {\n'
     '\t\tcompressor_pool_size = VM_COMPRESSOR_MAX_POOL_SIZE;\n'
     '\t}\n'
     '#if defined(MIDO_4K_VM)\n'
     '\t/* mido: the 4K kernel map has a ~46 GB hole; zones take 24 GB of it */\n'
     '\tif (vm_compression_limit == 0 && compressor_pool_size > 4ULL * max_mem) {\n'
     '\t\tcompressor_pool_size = 4ULL * max_mem;\n'
     '\t}\n'
     '#endif\n')

# ---------------------------------------------------------------------------
# sleh.c: EL0 emulation of ARMv8.1 LSE atomics. Cortex-A53 is ARMv8.0; the
# iOS 16 userland (A11 build) has ~860 inline LSE sites in libdispatch,
# libsystem_pthread/platform/malloc and dyld. They trap as uncategorized.
# With cpus=1 nothing else runs while interrupts are off, so the RMW is done
# directly on the user VA after AT S1E0W proved it is mapped writable (no PAN
# on MIDO); otherwise the page is faulted in for write and the check repeats.
# ---------------------------------------------------------------------------
MIDO_LSE = r'''
#if defined(MIDO_CPU_ARMV8_0)
static uint64_t mido_lse_count;

static inline uint64_t
mido_ureg(arm_saved_state_t *s, unsigned r)
{
	return r == 31 ? 0 : (uint64_t)get_saved_state_reg(s, r);
}

static inline void
mido_set_ureg(arm_saved_state_t *s, unsigned r, uint64_t v)
{
	if (r != 31) {
		set_saved_state_reg(s, r, (register_t)v);
	}
}

static inline bool
mido_user_writable(uint64_t va)
{
	uint64_t par;
	__asm__ volatile ("at s1e0w, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
	return (par & 1) == 0;
}

static inline uint64_t
mido_ld(uint64_t va, unsigned sz)
{
	switch (sz) {
	case 0: return *(volatile uint8_t *)va;
	case 1: return *(volatile uint16_t *)va;
	case 2: return *(volatile uint32_t *)va;
	default: return *(volatile uint64_t *)va;
	}
}

static inline void
mido_st(uint64_t va, unsigned sz, uint64_t v)
{
	switch (sz) {
	case 0: *(volatile uint8_t *)va = (uint8_t)v; break;
	case 1: *(volatile uint16_t *)va = (uint16_t)v; break;
	case 2: *(volatile uint32_t *)va = (uint32_t)v; break;
	default: *(volatile uint64_t *)va = v; break;
	}
}

static inline uint64_t
mido_trunc(uint64_t v, unsigned sz)
{
	return sz == 3 ? v : v & ((1ULL << (8u << sz)) - 1);
}

static inline int64_t
mido_sext(uint64_t v, unsigned sz)
{
	unsigned sh = 64 - (8u << sz);
	return (int64_t)(v << sh) >> sh;
}

/* Returns true if insn was an LSE atomic and has been emulated (PC advanced). */
static bool
mido_emulate_lse(arm_saved_state_t *state, uint32_t insn)
{
	unsigned kind, sz, op = 0, bytes;
	unsigned rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;

	if ((insn & 0x3F200C00) == 0x38200000) {               /* LD<op>, SWP */
		unsigned o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
		if (o3 && opc != 0) {
			return false;                           /* LDAPR (v8.3) and reserved */
		}
		kind = 0; sz = insn >> 30; op = o3 ? 8 : opc; bytes = 1u << sz;
	} else if ((insn & 0x3FA07C00) == 0x08A07C00) {        /* CAS{A,L,AL}{B,H,} */
		kind = 1; sz = insn >> 30; bytes = 1u << sz;
	} else if ((insn & 0xBFA07C00) == 0x08207C00) {        /* CASP{A,L,AL} */
		if ((rs & 1) || (rt & 1)) {
			return false;
		}
		kind = 2; sz = 2 + ((insn >> 30) & 1); bytes = 2u << sz;
	} else {
		return false;
	}

	uint64_t va = rn == 31 ? (uint64_t)get_saved_state_sp(state) : (uint64_t)get_saved_state_reg(state, rn);
	va = tbi_clear(va);
	vm_map_t map = current_map();
	if ((va & (bytes - 1)) || va < vm_map_min(map) || va + bytes > vm_map_max(map)) {
		return false;
	}

	for (int tries = 0; tries < 8; tries++) {
		boolean_t istate = ml_set_interrupts_enabled(FALSE);
		if (mido_user_writable(va)) {
			uint64_t a = mido_ureg(state, rs), old, nv = 0;
			__builtin_arm_dmb(0xb);
			if (kind == 0) {
				old = mido_ld(va, sz);
				switch (op) {
				case 0: nv = old + a; break;
				case 1: nv = old & ~a; break;
				case 2: nv = old ^ a; break;
				case 3: nv = old | a; break;
				case 4: nv = mido_sext(old, sz) > mido_sext(a, sz) ? old : a; break;
				case 5: nv = mido_sext(old, sz) < mido_sext(a, sz) ? old : a; break;
				case 6: nv = old > mido_trunc(a, sz) ? old : a; break;
				case 7: nv = old < mido_trunc(a, sz) ? old : a; break;
				default: nv = a; break;                 /* SWP */
				}
				mido_st(va, sz, nv);
				mido_set_ureg(state, rt, old);
			} else if (kind == 1) {
				old = mido_ld(va, sz);
				if (old == mido_trunc(a, sz)) {
					mido_st(va, sz, mido_ureg(state, rt));
				}
				mido_set_ureg(state, rs, old);
			} else {
				unsigned half = bytes / 2;
				uint64_t a1 = mido_ureg(state, rs + 1);
				uint64_t o0 = mido_ld(va, sz), o1 = mido_ld(va + half, sz);
				if (o0 == mido_trunc(a, sz) && o1 == mido_trunc(a1, sz)) {
					mido_st(va, sz, mido_ureg(state, rt));
					mido_st(va + half, sz, mido_ureg(state, rt + 1));
				}
				mido_set_ureg(state, rs, o0);
				mido_set_ureg(state, rs + 1, o1);
			}
			__builtin_arm_dmb(0xb);
			add_saved_state_pc(state, 4);
			ml_set_interrupts_enabled(istate);
			if (mido_lse_count++ == 0) {
				printf("MIDO LSE: emulating ARMv8.1 atomics for EL0 (first: %08x)\n", insn);
			}
			return true;
		}
		ml_set_interrupts_enabled(istate);
		vm_map_offset_t page = vm_map_trunc_page(va, PAGE_MASK);
		if (arm_fast_fault(map->pmap, page, VM_PROT_READ | VM_PROT_WRITE, FALSE, TRUE) != KERN_SUCCESS) {
			kern_return_t kr = vm_fault(map, page, VM_PROT_READ | VM_PROT_WRITE,
			    FALSE, VM_KERN_MEMORY_NONE, THREAD_ABORTSAFE, NULL, 0);
			if (kr != KERN_SUCCESS && kr != KERN_ABORTED) {
				return false;
			}
		}
	}
	return false;
}
#endif /* MIDO_CPU_ARMV8_0 */

static void
handle_uncategorized(arm_saved_state_t *state)
{
'''
edit('osfmk/arm64/sleh.c',
     '\nstatic void\nhandle_uncategorized(arm_saved_state_t *state)\n{\n',
     MIDO_LSE)
edit('osfmk/arm64/sleh.c',
     '\tCOPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));\n'
     '\n'
     '#if CONFIG_DTRACE\n',
     '\tCOPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));\n'
     '\n'
     '#if defined(MIDO_CPU_ARMV8_0)\n'
     '\tif (PSR64_IS_USER64(get_saved_state_cpsr(state)) && mido_emulate_lse(state, instr)) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '#endif\n'
     '\n'
     '#if CONFIG_DTRACE\n')

# ---------------------------------------------------------------------------
# sweet_ufs.c: eMMC transport for MIDO. Same driver above the transport
# (ufsroot0/rufsroot0, rd=ufsroot0, ufsrw=1 write guard, 4 MiB persistent
# console log at the end of the partition); below it, SDHCI PIO on the card
# the shim left selected (RCA 2, transfer state, sector addressing).
# The driver's 4 KiB "device blocks" are 8 eMMC sectors.
# ---------------------------------------------------------------------------
MIDO_EMMC = r'''
#if defined(MIDO_BOARD)
/*
 * MIDO: eMMC on SDHCI-MSM SDHC1 (vendor core 0x07824000, SDHCI at +0x900).
 * The mido shim re-identified the card (CMD0..CMD7, RCA 2) and left it in
 * the transfer state at 25 MHz, 1-bit bus, sector addressing. Polled PIO
 * through the buffer data port; CMD18/CMD25 with auto-CMD12 for multi-block.
 */
#define MIDO_SDHC_PAGE  0x07824000UL
#define MIDO_HC         0x900u
#define M_BLK           0x04
#define M_ARG           0x08
#define M_XFER          0x0c
#define M_RESP0         0x10
#define M_BUF           0x20
#define M_PRES          0x24
#define M_CLK           0x2c
#define M_STS           0x30
#define MS_CMD_DONE     (1u << 0)
#define MS_XFER_DONE    (1u << 1)
#define MS_ERR          (1u << 15)
#define MP_WR_EN        (1u << 10)
#define MP_RD_EN        (1u << 11)
#define MR_NONE         0x00u
#define MR_R1           0x1au
#define MR_R1B          0x1bu
#define MIDO_RCA        2u
#define MIDO_SECT       512u
#define MIDO_SPB        (UFS_BLOCK / MIDO_SECT)  /* sectors per driver block */
#define MIDO_CHUNK      256u                     /* sectors per command */

static uint32_t mido_resp;

static inline uint32_t
mh_rd(uint32_t o)
{
	return *(volatile uint32_t *)(su.regs + MIDO_HC + o);
}

static inline void
mh_wr(uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + MIDO_HC + o) = v;
}

static uint64_t
mido_deadline(uint64_t ms)
{
	uint64_t d;
	nanoseconds_to_absolutetime(ms * NSEC_PER_MSEC, &d);
	return mach_absolute_time() + d;
}

static void
mido_reset_lines(void)
{
	mh_wr(M_CLK, mh_rd(M_CLK) | (0x06u << 24));      /* reset CMD + DAT */
	uint64_t end = mido_deadline(100);
	while (((mh_rd(M_CLK) >> 24) & 0x06) && mach_absolute_time() < end) {
	}
	mh_wr(M_STS, 0xffffffff);
}

/* Wait for a status bit (or error); 0 on timeout. */
static uint32_t
mido_wait_sts(uint32_t mask, uint64_t ms)
{
	uint64_t end = mido_deadline(ms);
	do {
		uint32_t s = mh_rd(M_STS);
		if (s & (mask | MS_ERR)) {
			return s;
		}
	} while (mach_absolute_time() < end);
	/* Preempted past the deadline (run #25: CPU-starved)? Look once more. */
	uint32_t s = mh_rd(M_STS);
	return (s & (mask | MS_ERR)) ? s : 0;
}

/* One command, nblk 512-byte sectors of PIO data through buf. dir: 0, -1 read, 1 write. */
static int
mido_cmd(uint32_t idx, uint32_t arg, uint32_t rsp, int dir, uint32_t nblk, uint32_t *buf)
{
	uint64_t end = mido_deadline(1000);
	while (mh_rd(M_PRES) & ((dir || rsp == MR_R1B) ? 3u : 1u)) {
		if (mach_absolute_time() > end) {
			printf("MIDO EMMC: CMD%u: lines busy (pres 0x%x)\n", idx, mh_rd(M_PRES));
			mido_reset_lines();
			return EBUSY;
		}
	}
	mh_wr(M_STS, 0xffffffff);
	uint32_t xfer = 0, c = (idx << 8) | rsp;
	if (dir) {
		mh_wr(M_BLK, (nblk << 16) | MIDO_SECT);
		xfer = (1u << 1) | (dir < 0 ? (1u << 4) : 0);   /* block count enable, read */
		if (nblk > 1) {
			xfer |= (1u << 5) | (1u << 2);           /* multi-block, auto-CMD12 */
		}
		c |= 0x20;                                       /* data present */
	}
	mh_wr(M_ARG, arg);
	mh_wr(M_XFER, (c << 16) | xfer);

	uint32_t s = mido_wait_sts(MS_CMD_DONE, 1000);
	if (!s || (s & MS_ERR)) {
		printf("MIDO EMMC: CMD%u arg 0x%x: %s (sts 0x%08x)\n", idx, arg,
		    s ? "error" : "timeout", s);
		mido_reset_lines();
		return s ? EIO : ETIMEDOUT;
	}
	mido_resp = mh_rd(M_RESP0);
	mh_wr(M_STS, MS_CMD_DONE);

	for (uint32_t b = 0; dir && b < nblk; b++) {
		uint32_t ready = dir < 0 ? MP_RD_EN : MP_WR_EN;
		uint64_t bend = mido_deadline(3000);
		for (;;) {
			s = mh_rd(M_STS);
			if (s & MS_ERR) {
				break;
			}
			if (mh_rd(M_PRES) & ready) {
				break;
			}
			if (mach_absolute_time() > bend) {
				/* run #25: "data timeout (sts 0x00008000)" with no hardware error
				 * bits = this deadline passing while preempted. Re-check first. */
				s = (mh_rd(M_PRES) & ready) ? 0 : MS_ERR;
				break;
			}
		}
		if (s & MS_ERR) {
			printf("MIDO EMMC: CMD%u block %u/%u: data %s (sts 0x%08x)\n", idx, b, nblk,
			    (s >> 16) ? "error" : "timeout", s);
			mido_reset_lines();
			return EIO;
		}
		mh_wr(M_STS, (1u << 4) | (1u << 5));             /* buffer read/write ready */
		uint32_t *p = buf + b * (MIDO_SECT / 4);
		if (dir < 0) {
			for (unsigned i = 0; i < MIDO_SECT / 4; i++) {
				p[i] = mh_rd(M_BUF);
			}
		} else {
			for (unsigned i = 0; i < MIDO_SECT / 4; i++) {
				mh_wr(M_BUF, p[i]);
			}
		}
	}
	if (dir || rsp == MR_R1B) {
		s = mido_wait_sts(MS_XFER_DONE, 5000);
		if (!s || (s & MS_ERR)) {
			printf("MIDO EMMC: CMD%u: transfer %s (sts 0x%08x)\n", idx,
			    s ? "error" : "timeout", s);
			mido_reset_lines();
			return s ? EIO : ETIMEDOUT;
		}
	}
	mh_wr(M_STS, 0xffffffff);
	return 0;
}

/* CMD13 until the card is in transfer state and ready for data. */
static int
mido_card_ready(uint64_t ms)
{
	uint64_t end = mido_deadline(ms);
	do {
		int e = mido_cmd(13, MIDO_RCA << 16, MR_R1, 0, 0, NULL);
		if (e) {
			return e;
		}
		if (((mido_resp >> 9) & 0xf) == 4 && (mido_resp & (1u << 8))) {
			return 0;
		}
	} while (mach_absolute_time() < end);
	printf("MIDO EMMC: card not ready (status 0x%x)\n", mido_resp);
	return ETIMEDOUT;
}

/*
 * After a failed command: reset the CMD/DAT lines, stop an open multi-block
 * transfer (auto-CMD12 may not have gone out after a data error) and wait
 * for the card to return to the transfer state.
 */
static int
mido_recover(void)
{
	mido_reset_lines();
	if (mido_cmd(13, MIDO_RCA << 16, MR_R1, 0, 0, NULL) == 0) {
		uint32_t st = (mido_resp >> 9) & 0xf;
		if (st == 5 || st == 6) {                        /* data / rcv */
			(void)mido_cmd(12, 0, MR_R1B, 0, 0, NULL);
		}
	}
	return mido_card_ready(5000);
}

/*
 * nsec sectors at sector into / from the bounce buffer. A failed chunk is
 * retried after recovery (reads and writes of the same data are idempotent).
 * Build #17: a CMD25 failed with SDHCI data timeout (sts 0x00108002) while the
 * card was busy programming, which cost an HFS journal transaction.
 */
static int
mido_xfer(uint64_t sector, uint32_t nsec, bool write)
{
	uint32_t *buf = (uint32_t *)(su.dma + DMA_BOUNCE);
	if (nsec > BOUNCE_SIZE / MIDO_SECT || sector + nsec > (1ULL << 32)) {
		return EINVAL;
	}
	for (uint32_t done = 0; done < nsec;) {
		uint32_t n = MIN(nsec - done, MIDO_CHUNK);
		uint32_t idx = write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
		int e = 0;
		for (int attempt = 0; attempt < 4; attempt++) {
			e = mido_cmd(idx, (uint32_t)(sector + done), MR_R1, write ? 1 : -1, n,
			    buf + done * (MIDO_SECT / 4));
			if (!e && write) {
				e = mido_card_ready(5000);
			}
			if (!e) {
				if (attempt) {
					printf("MIDO EMMC: CMD%u at 0x%llx x%u ok on retry %d\n", idx,
					    sector + done, n, attempt);
				}
				break;
			}
			int r = mido_recover();
			printf("MIDO EMMC: CMD%u at 0x%llx x%u failed (%d), recover %d, retry %d\n",
			    idx, sector + done, n, e, r, attempt + 1);
		}
		if (e) {
			return e;
		}
		done += n;
	}
	return 0;
}

static int
mido_read_blocks(uint64_t lba, uint32_t count)
{
	if (count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
	return mido_xfer(lba * MIDO_SPB, count * MIDO_SPB, false);
}

/* Only called from ufs_write_blocks after its partition bounds check. */
static int
mido_write_blocks(uint64_t lba, uint32_t count)
{
	return mido_xfer(lba * MIDO_SPB, count * MIDO_SPB, true);
}

static int
mido_sync_cache(void)
{
	static bool warned;
	/* SWITCH (write byte) EXT_CSD[32] FLUSH_CACHE = 1; harmless with the cache off. */
	int e = mido_cmd(6, (3u << 24) | (32u << 16) | (1u << 8), MR_R1B, 0, 0, NULL);
	if (!e) {
		e = mido_card_ready(2000);
	}
	if (e && !warned) {
		warned = true;
		printf("MIDO EMMC: cache flush failed (%d), ignoring\n", e);
	}
	return 0;
}

static int
mido_take_over(void)
{
	/* Data timeout counter (CLK reg [19:16]) to the maximum, 0xE = TMCLK x 2^27:
	 * long write-busy (programming) phases must not abort CMD24/CMD25. */
	mh_wr(M_CLK, (mh_rd(M_CLK) & ~(0xfu << 16)) | (0xeu << 16));
	int e = mido_card_ready(500);
	if (e) {
		printf("MIDO EMMC: card not in transfer state after the shim (%d)\n", e);
		return ENXIO;
	}
	return 0;
}

static int
mido_find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	int error = mido_xfer(1, 1, false);
	if (error) {
		return error;
	}
	if (memcmp(b, "EFI PART", 8) != 0) {
		printf("MIDO EMMC: no GPT\n");
		return ENXIO;
	}
	uint64_t entry_lba;
	uint32_t nent, esz;
	memcpy(&entry_lba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	if (esz != 128 || nent == 0 || nent * esz > BOUNCE_SIZE) {
		printf("MIDO EMMC: unsupported GPT entries %u x %u\n", nent, esz);
		return ENXIO;
	}
	error = mido_xfer(entry_lba, (nent * esz + MIDO_SECT - 1) / MIDO_SECT, false);
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = 0;
		if (strcmp(name, want) != 0) {
			continue;
		}
		uint64_t first, last;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (last < first || (first % MIDO_SPB)) {
			printf("MIDO EMMC: \"%s\" sectors 0x%llx-0x%llx not 4 KiB aligned\n", want, first, last);
			return ENXIO;
		}
		su.first_block = first / MIDO_SPB;
		su.nblocks = (last - first + 1) / MIDO_SPB;
		return 0;
	}
	printf("MIDO EMMC: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}
#endif /* MIDO_BOARD */
'''
UFS = 'bsd/dev/sweet_ufs.c'
edit(UFS,
     'static LCK_MTX_DECLARE(sweet_ufs_lock, &sweet_ufs_grp);\n',
     'static LCK_MTX_DECLARE(sweet_ufs_lock, &sweet_ufs_grp);\n' + MIDO_EMMC)
# The UFS-only helpers are unused on MIDO.
edit(UFS, 'static void\ndma_sync(', 'static void __unused\ndma_sync(')
edit(UFS, 'static void\ndma_clean(', 'static void __unused\ndma_clean(')
edit(UFS, 'static int\nufs_scsi(const uint8_t *cdb', 'static int __unused\nufs_scsi(const uint8_t *cdb')
# read
edit(UFS,
     'ufs_read_blocks(uint64_t lba, uint32_t count)\n{\n',
     'ufs_read_blocks(uint64_t lba, uint32_t count)\n{\n'
     '#if defined(MIDO_BOARD)\n\treturn mido_read_blocks(lba, count);\n#else\n')
edit(UFS,
     '\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);\n}\n',
     '\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);\n#endif\n}\n')
# write (after the partition bounds check, which stays)
edit(UFS,
     '\tuint8_t cdb[10] = {\n\t\t0x2a, 0,\n',
     '#if defined(MIDO_BOARD)\n\treturn mido_write_blocks(lba, count);\n#else\n'
     '\tuint8_t cdb[10] = {\n\t\t0x2a, 0,\n')
edit(UFS,
     '\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);\n}\n',
     '\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);\n#endif\n}\n')
# sync
edit(UFS,
     '\tstatic const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */\n'
     '\treturn ufs_scsi(cdb, sizeof(cdb), 0, false);\n',
     '#if defined(MIDO_BOARD)\n\treturn mido_sync_cache();\n#else\n'
     '\tstatic const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */\n'
     '\treturn ufs_scsi(cdb, sizeof(cdb), 0, false);\n#endif\n')
# GPT
edit(UFS,
     'ufs_find_partition(const char *want)\n{\n',
     'ufs_find_partition(const char *want)\n{\n'
     '#if defined(MIDO_BOARD)\n\treturn mido_find_partition(want);\n#else\n')
edit(UFS,
     '\tprintf("SWEET UFS: no GPT partition named \\"%s\\"\\n", want);\n\treturn ENOENT;\n}\n',
     '\tprintf("SWEET UFS: no GPT partition named \\"%s\\"\\n", want);\n\treturn ENOENT;\n#endif\n}\n')
# host take-over
edit(UFS,
     'ufs_take_over_host(void)\n{\n',
     'ufs_take_over_host(void)\n{\n'
     '#if defined(MIDO_BOARD)\n\treturn mido_take_over();\n#else\n')
edit(UFS,
     '\t\tprintf("SWEET UFS: host did not take our request list\\n");\n'
     '\t\treturn EIO;\n\t}\n\treturn 0;\n}\n',
     '\t\tprintf("SWEET UFS: host did not take our request list\\n");\n'
     '\t\treturn EIO;\n\t}\n\treturn 0;\n#endif\n}\n')
# init: no /sweet-ufs node, fixed SDHC address, no SCSI TEST UNIT READY
edit(UFS,
     '\tif (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||\n',
     '#if defined(MIDO_BOARD)\n'
     '\t(void)node;\n\t(void)reg;\n\t(void)size;\n'
     '\tif (0) {\n'
     '#else\n'
     '\tif (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||\n')
edit(UFS,
     '\t    size < 2 * sizeof(uintptr_t)) {\n'
     '\t\tprintf("SWEET UFS: no /sweet-ufs node in the device tree\\n");\n',
     '\t    size < 2 * sizeof(uintptr_t)) {\n'
     '#endif\n'
     '\t\tprintf("SWEET UFS: no /sweet-ufs node in the device tree\\n");\n')
edit(UFS,
     '\tsu.regs = ml_io_map(reg[0], reg[1]);\n',
     '#if defined(MIDO_BOARD)\n'
     '\tsu.regs = ml_io_map(MIDO_SDHC_PAGE, 0x1000);\n'
     '#else\n'
     '\tsu.regs = ml_io_map(reg[0], reg[1]);\n'
     '#endif\n')
edit(UFS,
     '\tif (!error) {\n'
     '\t\tstatic const uint8_t tur[6] = { 0x00 };\n',
     '#if !defined(MIDO_BOARD)\n'
     '\tif (!error) {\n'
     '\t\tstatic const uint8_t tur[6] = { 0x00 };\n')
edit(UFS,
     '\t\tfor (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {\n'
     '\t\t}\n'
     '\t}\n',
     '\t\tfor (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {\n'
     '\t\t}\n'
     '\t}\n'
     '#endif\n')
# The SD730 button/touch/USB drivers poke sweet-only hardware; mido has its own
# read-only input poller (bsd/dev/mido_input.c, set up by the shim).
edit(UFS,
     '\tsweet_keys_init();\n\tsweet_touch_init();\n\tsweet_usb_probe();\n',
     '#if !defined(MIDO_BOARD)\n'
     '\tsweet_keys_init();\n\tsweet_touch_init();\n\tsweet_usb_probe();\n'
     '#else\n'
     '\t{\n\t\textern void mido_input_init(void);\n\t\tmido_input_init();\n\t}\n'
     '\t{\n\t\textern void mido_gpu_init(void);\n\t\tmido_gpu_init();\n\t}\n'
     '#endif\n')

# ---------------------------------------------------------------------------
# Boot progress diagnostics (build #12). Build #11 stops after the SWEET IOKit
# drivers and never prints bsd_init's copyright: kernel_bootstrap_thread is
# stuck between trust_cache_init and bsd_init. Print the bootstrap stages and
# bsd_init steps on the console, and a 5 s heartbeat (thread_call) naming the
# last bootstrap stage, which also shows whether timer wakeups still work.
# ---------------------------------------------------------------------------
edit('osfmk/kern/startup.c',
     'static inline void\nkernel_bootstrap_log(const char *message)\n{\n',
     '#if defined(MIDO_BOARD)\n'
     '#include <kern/thread_call.h>\n'
     '/* thread_kernel_state_t: kern/thread_kernel_state.h is already included (and has no #define guard) */\n'
     'static const char *volatile mido_boot_stage = "start";\n'
     'static thread_call_t mido_tick_call;\n'
     'static uint32_t mido_ticks;\n'
     'static thread_t mido_boot_thread;\n'
     '\n'
     '/* State, wait event and frame-pointer backtrace of a switched-out kernel thread. */\n'
     'static void\n'
     'mido_dump_thread(thread_t t)\n'
     '{\n'
     '\tif (t == NULL || t == current_thread() || !t->machine.kstackptr || !t->kernel_stack) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tarm_kernel_saved_state_t *ss = &((thread_kernel_state_t)t->machine.kstackptr)->machine.ss;\n'
     '\tuint64_t lo = t->kernel_stack, hi = lo + kernel_stack_size, fp = ss->fp;\n'
     '\tprintf("MIDO boot thread: state 0x%x wait %p lr %llx\\n", t->state,\n'
     '\t    (void *)t->wait_event, ss->lr);\n'
     '\tprintf("MIDO bt:");\n'
     '\tfor (int i = 0; i < 16 && fp >= lo && fp + 16 <= hi && !(fp & 7); i++) {\n'
     '\t\tconst uint64_t *f = (const uint64_t *)fp;\n'
     '\t\tprintf(" %llx", f[1]);\n'
     '\t\tif ((i & 3) == 3) {\n'
     '\t\t\tprintf("\\n       ");\n'
     '\t\t}\n'
     '\t\tfp = f[0];\n'
     '\t}\n'
     '\tprintf("\\n");\n'
     '}\n'
     '\n'
     'static void\n'
     'mido_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)\n'
     '{\n'
     '\tuint64_t deadline;\n'
     '\tstatic const char *last_stage;\n'
     '\tconst char *stage = mido_boot_stage;\n'
     '\tbool in_bsd = (strcmp(stage, "bsd_init") == 0);\n'
     '\t++mido_ticks;\n'
     '\t/* Quiet once the bootstrap thread has become init: one line a minute. */\n'
     '\tif (stage != last_stage || (mido_ticks % 12) == 0) {\n'
     '\t\tprintf("MIDO tick %u: bootstrap at %s\\n", mido_ticks, stage);\n'
     '\t}\n'
     '\tif (!in_bsd && stage == last_stage && mido_ticks >= 3) {\n'
     '\t\tmido_dump_thread(mido_boot_thread);\n'
     '\t}\n'
     '\tlast_stage = stage;\n'
     '\tclock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);\n'
     '\tthread_call_enter_delayed(mido_tick_call, deadline);\n'
     '}\n'
     '#endif\n'
     '\n'
     'static inline void\nkernel_bootstrap_log(const char *message)\n{\n'
     '#if defined(MIDO_BOARD)\n'
     '\tmido_boot_stage = message;\n'
     '\tif (startup_phase >= STARTUP_SUB_KPRINTF) {\n'
     '\t\tprintf("KB: %s\\n", message);\n'
     '\t}\n'
     '#endif\n')
edit('osfmk/kern/startup.c',
     'static inline void\nkernel_bootstrap_thread_log(const char *message)\n{\n',
     'static inline void\nkernel_bootstrap_thread_log(const char *message)\n{\n'
     '#if defined(MIDO_BOARD)\n'
     '\tmido_boot_stage = message;\n'
     '\tprintf("KBT: %s\\n", message);\n'
     '#endif\n')
edit('osfmk/kern/startup.c',
     '\tkernel_startup_initialize_upto(STARTUP_SUB_THREAD_CALL);\n',
     '\tkernel_startup_initialize_upto(STARTUP_SUB_THREAD_CALL);\n'
     '#if defined(MIDO_BOARD)\n'
     '\t{\n'
     '\t\tuint64_t deadline;\n'
     '\t\tuint32_t mute = 0;\n'
     '\t\t/* mido_mute=1: stop drawing console text from here on (it still goes to the\n'
     '\t\t * persistent log; panics draw). sweetfb_noconsole only mutes at the first GUI swap. */\n'
     '\t\tif (PE_parse_boot_argn("mido_mute", &mute, sizeof(mute)) && mute) {\n'
     '\t\t\textern int sweet_console_muted;\n'
     '\t\t\tprintf("MIDO: console muted (mido_mute=1); see the persistent log\\n");\n'
     '\t\t\tsweet_console_muted = 1;\n'
     '\t\t}\n'
     '\t\tmido_boot_thread = current_thread();\n'
     '\t\tmido_tick_call = thread_call_allocate(mido_tick, NULL);\n'
     '\t\tclock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);\n'
     '\t\tthread_call_enter_delayed(mido_tick_call, deadline);\n'
     '\t}\n'
     '#endif\n')
edit('osfmk/kern/startup.c',
     '\tPE_lockdown_iokit();\n#endif\n',
     '\tPE_lockdown_iokit();\n'
     '#if defined(MIDO_BOARD)\n\tkernel_bootstrap_log("PE_lockdown_iokit done");\n#endif\n'
     '#endif\n')
edit('osfmk/kern/startup.c',
     '#ifdef  MACH_BSD\n\tbsd_init();\n#endif\n',
     '#if defined(MIDO_BOARD)\n\tkernel_bootstrap_log("bsd_init");\n#endif\n'
     '#ifdef  MACH_BSD\n\tbsd_init();\n#endif\n')
edit('bsd/kern/bsd_init.c',
     '#if DEBUG_BSDINIT\n#define bsd_init_kprintf(x, ...) kprintf("bsd_init: " x, ## __VA_ARGS__)\n',
     '#if defined(MIDO_BOARD)\n#define bsd_init_kprintf(x, ...) printf("bsd_init: " x, ## __VA_ARGS__)\n'
     '#elif DEBUG_BSDINIT\n#define bsd_init_kprintf(x, ...) kprintf("bsd_init: " x, ## __VA_ARGS__)\n')

# arm_init.c: > 1 GB RAM selects 16 KB VM pages over the 4 KB granule (the A9
# PAGE_RATIO 4 mode). That is what iOS 16 userland needs: the dyld shared cache is
# code-signed with 16 KB hash pages (CD pageShift 14), which a 4 KB kernel rejects
# ("disallowing unsupported code signature page shift"), so dyld finds "no dyld
# cache". Keep XNU's choice (16 KB on mido's 1.5 GB); boot-arg mido_vm4k=1 forces
# 4 KB VM pages = hardware pages (the <= 1 GB path).
edit('osfmk/arm/arm_init.c',
     '\t\tif (args->memSize > 1ULL * 1024 * 1024 * 1024) {\n',
     '#if defined(MIDO_BOARD)\n'
     '\t\tuint32_t mido_vm4k = 0;\n'
     '\t\tPE_parse_boot_argn("mido_vm4k", &mido_vm4k, sizeof(mido_vm4k));\n'
     '\t\tif (!mido_vm4k && args->memSize > 1ULL * 1024 * 1024 * 1024) {\n'
     '#else\n'
     '\t\tif (args->memSize > 1ULL * 1024 * 1024 * 1024) {\n'
     '#endif\n')

# pmap.c: with PAGE_RATIO 4 (16 KB VM pages, 4 KB PTEs) pmap_create_commpages picks
# the commpage text VA from any 4 KB leaf slot, but pmap_enter -> write_pte_fast
# writes 4 PTEs and needs a 16 KB-aligned VA (ptep 32-byte aligned): build #12
# panicked "write_pte_fast: PTE write is unaligned" from vm_commpage_init.
# Pick only slots that start a VM page.
edit('osfmk/arm/pmap/pmap.c',
     '\t\tint text_leaf_index = random() % num_ptes;\n',
     '\t\tint text_leaf_index = random() % num_ptes;\n'
     '#if defined(MIDO_BOARD)\n'
     '\t\ttext_leaf_index &= ~(PAGE_RATIO - 1);   /* VM-page aligned for PAGE_RATIO > 1 */\n'
     '#endif\n')

# ---------------------------------------------------------------------------
# CPU driver: build #15's heartbeat backtrace showed kernel_bootstrap_thread
# asleep in ml_wait_max_cpus() (from vm_set_restrictions). ml_set_max_cpus() is
# called by SweetCPU::startAll, but SweetCPU never started: its personality
# matches 'qcom,kryo470' and the mido ADT CPU nodes are 'arm,cortex-a53'.
# Its MPIDR check also assumes sweet's layout (Aff0 = 0); mido is 0x000-0x003
# (cluster 0) and 0x100-0x103 (cluster 1).
# ---------------------------------------------------------------------------
edit('iokit/KernelConfigTables.cpp',
     "    \"     'IONameMatch'     = 'qcom,kryo470';\"\n",
     '#if defined(MIDO_BOARD)\n'
     "    \"     'IONameMatch'     = 'arm,cortex-a53';\"\n"
     '#else\n'
     "    \"     'IONameMatch'     = 'qcom,kryo470';\"\n"
     '#endif\n')
edit('iokit/Kernel/arm/SweetCPU.cpp',
     '\tif (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||\n',
     '#if defined(MIDO)\n'
     '\t/* MSM8953: 2 clusters x 4 Cortex-A53, MPIDR Aff1 = cluster, Aff0 = core */\n'
     '\tif ((cpu->phys_id & ~0x103u) != 0 ||\n'
     '#else\n'
     '\tif (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||\n'
     '#endif\n')
edit('iokit/Kernel/arm/SweetCPU.cpp',
     '\tcpuName = OSSymbol::withCStringNoCopy("Kryo 470");\n',
     '#if defined(MIDO)\n'
     '\tcpuName = OSSymbol::withCStringNoCopy("Cortex-A53");\n'
     '#else\n'
     '\tcpuName = OSSymbol::withCStringNoCopy("Kryo 470");\n'
     '#endif\n')

# ---------------------------------------------------------------------------
# SweetFramebuffer: present the iPhone 8 Plus canvas and downsample on the CPU.
# Build #18's log: backboardd renders at the D211 canvas (surface 1242x2208),
# SWEET FB presents the iPhone X panel (1125x2436, patch 0061), so backboardd
# wants a hardware scaler: "IOServiceMatching failed for: AppleM2ScalerCSCDriver"
# x9838 and no frame is ever presented. Under MIDO the display reports
# 1242x2208 and AppleCLCD::present() maps each panel pixel (1080x1920) back to
# its canvas pixel (nearest neighbour, 16.16 fixed point; 0.87x both axes, the
# same ratio as the real 8 Plus downsampling).
# ---------------------------------------------------------------------------
FB = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(FB,
     '\tint32_t       fOffX, fOffY;             /* logical origin of the panel */\n',
     '\tint32_t       fOffX, fOffY;             /* logical origin of the panel */\n'
     '#if defined(MIDO_BOARD)\n'
     '\tbool          fScaled;          /* logical canvas downsampled to the panel */\n'
     '\tvoid          midoPresentScaled(SweetSurface *s, int32_t sx, int32_t sy,\n'
     '\t    int32_t dx, int32_t dy, int32_t w, int32_t h);\n'
     '#endif\n')
edit(FB,
     '\tfOffX = (int32_t)(fLogicalW - fWidth) / 2;\n'
     '\tfOffY = (int32_t)(fLogicalH - fHeight) / 2;\n',
     '\tfOffX = (int32_t)(fLogicalW - fWidth) / 2;\n'
     '\tfOffY = (int32_t)(fLogicalH - fHeight) / 2;\n'
     '#if defined(MIDO_BOARD)\n'
     '\t/* iPhone 8 Plus (D211) canvas, 414x736 points @3x */\n'
     '\tfScaled = !native;\n'
     '\tif (fScaled) {\n'
     '\t\tfLogicalW = 1242;\n'
     '\t\tfLogicalH = 2208;\n'
     '\t\tfOffX = fOffY = 0;\n'
     '\t}\n'
     '#endif\n')
edit(FB,
     '\t\t/* Logical (presented) display coordinates -> panel coordinates. */\n'
     '\t\tdx -= fOffX;\n',
     '#if defined(MIDO_BOARD)\n'
     '\t\tif (fScaled) {\n'
     '\t\t\tif (bpe == 4) {\n'
     '\t\t\t\tmidoPresentScaled(s, sx, sy, dx, dy, w, h);\n'
     '\t\t\t}\n'
     '\t\t\ts->release();\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '#endif\n'
     '\t\t/* Logical (presented) display coordinates -> panel coordinates. */\n'
     '\t\tdx -= fOffX;\n')
edit(FB,
     'bool\nIOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)\n',
     '#if defined(MIDO_BOARD)\n'
     '/*\n'
     ' * Copy canvas rect (sx,sy)+(w,h) shown at canvas (dx,dy) into the panel,\n'
     ' * nearest neighbour: panel pixel p shows canvas pixel p * canvas / panel.\n'
     ' */\n'
     'void\n'
     'AppleCLCD::midoPresentScaled(SweetSurface *s, int32_t sx, int32_t sy,\n'
     '    int32_t dx, int32_t dy, int32_t w, int32_t h)\n'
     '{\n'
     '\tif (s->elementWidth != 1 || s->elementHeight != 1 || s->memory == NULL) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tif (w > (int32_t)s->width - sx) {\n'
     '\t\tw = (int32_t)s->width - sx;\n'
     '\t}\n'
     '\tif (h > (int32_t)s->height - sy) {\n'
     '\t\th = (int32_t)s->height - sy;\n'
     '\t}\n'
     '\tif (w > (int32_t)fLogicalW - dx) {\n'
     '\t\tw = (int32_t)fLogicalW - dx;\n'
     '\t}\n'
     '\tif (h > (int32_t)fLogicalH - dy) {\n'
     '\t\th = (int32_t)fLogicalH - dy;\n'
     '\t}\n'
     '\tif (w <= 0 || h <= 0) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tuint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +\n'
     '\t    (uint64_t)(sx + w) * 4;\n'
     '\tif (last > s->memory->getLength()) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tuintptr_t base = s->kernelAddress() + s->offset;\n'
     '\tuint32_t stepx = (fLogicalW << 16) / fWidth, stepy = (fLogicalH << 16) / fHeight;\n'
     '\t/*\n'
     '\t * backboardd renders the D211 (P3 wide colour) UI in kCVPixelFormatType_30RGBLE_8A\n'
     '\t * (\'b3a8\', 2 planes): plane 0 words are R[29:20] G[19:10] B[9:0], extended range\n'
     '\t * 384 = 0.0 .. 895 = 1.0; alpha is in plane 1 (ignored, the result is opaque).\n'
     '\t * Copied raw as BGRA, black came out navy and white pale yellow (run #22 lock screen).\n'
     '\t */\n'
     '\tstatic uint8_t xr10[1024];\n'
     '\tstatic bool xr10_ready;\n'
     '\tbool wide = (s->pixelFormat == 0x62336138u);   /* \'b3a8\' */\n'
     '\tif (wide && !xr10_ready) {\n'
     '\t\tfor (int v = 0; v < 1024; v++) {\n'
     '\t\t\tint c = ((v - 384) * 255 + 255) / 511;\n'
     '\t\t\txr10[v] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);\n'
     '\t\t}\n'
     '\t\txr10_ready = true;\n'
     '\t\tIOLog("SWEET FB: converting b3a8 (30RGBLE_8A, XR 384..895) surfaces to BGRA\\n");\n'
     '\t}\n'
     '\t/* panel range whose canvas pixel falls inside [d, d + len) */\n'
     '\tuint32_t px0 = (uint32_t)(((uint64_t)dx << 16) + stepx - 1) / stepx;\n'
     '\tuint32_t px1 = (uint32_t)(((uint64_t)(dx + w) << 16) + stepx - 1) / stepx;\n'
     '\tuint32_t py0 = (uint32_t)(((uint64_t)dy << 16) + stepy - 1) / stepy;\n'
     '\tuint32_t py1 = (uint32_t)(((uint64_t)(dy + h) << 16) + stepy - 1) / stepy;\n'
     '\tpx1 = MIN(px1, fWidth);\n'
     '\tpy1 = MIN(py1, fHeight);\n'
     '\tfor (uint32_t py = py0; py < py1; py++) {\n'
     '\t\tint32_t ly = (int32_t)((py * stepy) >> 16) - dy;\n'
     '\t\tif (ly < 0 || ly >= h) {\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '\t\tconst uint32_t *srow = (const uint32_t *)(base + (uint64_t)(sy + ly) * s->bytesPerRow) + sx;\n'
     '\t\tuint32_t *drow = (uint32_t *)(fPixels + (uint64_t)py * fRowBytes);\n'
     '\t\tfor (uint32_t px = px0; px < px1; px++) {\n'
     '\t\t\tint32_t lx = (int32_t)((px * stepx) >> 16) - dx;\n'
     '\t\t\tif (lx >= 0 && lx < w) {\n'
     '\t\t\t\tuint32_t v = srow[lx];\n'
     '\t\t\t\tif (wide) {\n'
     '\t\t\t\t\tv = 0xff000000u | (uint32_t)xr10[(v >> 20) & 0x3ff] << 16 |\n'
     '\t\t\t\t\t    (uint32_t)xr10[(v >> 10) & 0x3ff] << 8 | xr10[v & 0x3ff];\n'
     '\t\t\t\t}\n'
     '\t\t\t\tdrow[px] = v;\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n'
     '}\n'
     '#endif\n'
     '\n'
     'bool\nIOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)\n')

# ---------------------------------------------------------------------------
# Input: FT5435 touch + power/volume/home keys (bsd/dev/mido_input.c, read-only;
# the shim's input_setup powers and muxes the hardware). SweetHID normalises
# touches against the presented display: on mido the whole 1080x1920 panel is
# the (downsampled) 1242x2208 canvas, so normalise against the panel itself.
# ---------------------------------------------------------------------------
create('bsd/dev/mido_input.c',
       (Path(__file__).resolve().parent / 'src' / 'mido_input.c').read_text())
edit('bsd/conf/files',
     'bsd/dev/sweet_touch.c\t\t\tstandard\n',
     'bsd/dev/sweet_touch.c\t\t\tstandard\n'
     'bsd/dev/mido_input.c\t\t\tstandard\n'
     'bsd/dev/mido_gpu.c\t\t\tstandard\n')
# Adreno 506 bring-up (bsd/dev/mido_gpu.c, boot-arg midogpu=1; firmware via mkpayload --blob)
create('bsd/dev/mido_gpu.c',
       (Path(__file__).resolve().parent / 'src' / 'mido_gpu.c').read_text())
HID = 'iokit/Kernel/arm/hid/SweetHID.cpp'
edit(HID,
     '\tfLogicalW = native ? 1080 : 1125;\n'
     '\tfLogicalH = native ? 2400 : 2436;\n'
     '\tfOffX = (fLogicalW - 1080) / 2;\n'
     '\tfOffY = (fLogicalH - 2400) / 2;\n',
     '#if defined(MIDO_BOARD)\n'
     '\t(void)native;\n'
     '\tfLogicalW = 1080;                   /* panel = whole downsampled canvas */\n'
     '\tfLogicalH = 1920;\n'
     '\tfOffX = fOffY = 0;\n'
     '#else\n'
     '\tfLogicalW = native ? 1080 : 1125;\n'
     '\tfLogicalH = native ? 2400 : 2436;\n'
     '\tfOffX = (fLogicalW - 1080) / 2;\n'
     '\tfOffY = (fLogicalH - 2400) / 2;\n'
     '#endif\n')
edit(HID,
     '\ty = y < 0 ? 0 : (y > 2399 ? 2399 : y);\n',
     '#if defined(MIDO_BOARD)\n'
     '\ty = y < 0 ? 0 : (y > 1919 ? 1919 : y);\n'
     '#else\n'
     '\ty = y < 0 ? 0 : (y > 2399 ? 2399 : y);\n'
     '#endif\n')
edit(HID,
     '\tsetProperty("SurfaceHeight", (unsigned long long)(151 << 16), 32);\n',
     '#if defined(MIDO_BOARD)\n'
     '\tsetProperty("SurfaceHeight", (unsigned long long)(121 << 16), 32);	/* 5.5" 16:9 */\n'
     '#else\n'
     '\tsetProperty("SurfaceHeight", (unsigned long long)(151 << 16), 32);\n'
     '#endif\n')

# ---------------------------------------------------------------------------
# Performance (run #25 boot: input events reach the kernel at once but iOS reacts
# after minutes; sweetfb_hz=1 did not help). Without a GPU, backboardd software-
# renders on every vsync notification, which SWEET FB sends at a fixed 60 Hz:
# >3000 swaps. MIDO default 15 Hz (boot-arg mido_vsync_hz), panel copy per vsync.
# ---------------------------------------------------------------------------
edit(FB,
     '#define SWEET_FB_VSYNC_US               16667\n',
     '#if defined(MIDO_BOARD)\n'
     'static uint32_t mido_vsync_us = 66667;          /* 15 Hz, boot-arg mido_vsync_hz */\n'
     '#define SWEET_FB_VSYNC_US               mido_vsync_us\n'
     '#else\n'
     '#define SWEET_FB_VSYNC_US               16667\n'
     '#endif\n')
edit(FB,
     '\tuint32_t hz = 10;\n'
     '\tPE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));\n'
     '\tfFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;\n',
     '#if defined(MIDO_BOARD)\n'
     '\tuint32_t vhz = 15, hz = 15;\n'
     '\tPE_parse_boot_argn("mido_vsync_hz", &vhz, sizeof(vhz));\n'
     '\tif (vhz == 0 || vhz > 60) {\n'
     '\t\tvhz = 15;\n'
     '\t}\n'
     '\tmido_vsync_us = 1000000u / vhz;\n'
     '\tPE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));\n'
     '\tfFlushTicks = (hz == 0 || hz >= vhz) ? 1 : vhz / hz;\n'
     '\tIOLog("SWEET FB: MIDO vsync %u Hz, panel copy every %u vsync\\n", vhz, fFlushTicks);\n'
     '#else\n'
     '\tuint32_t hz = 10;\n'
     '\tPE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));\n'
     '\tfFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;\n'
     '#endif\n')

# LSE emulation statistics: per-process counts (8-slot table, reset each report),
# printed once a minute by the heartbeat. logd_helper once burned 90 s of system time
# (0.1 s user) - an LSE spin storm would look exactly like that.
edit('osfmk/arm64/sleh.c',
     'static uint64_t mido_lse_count;\n',
     'static uint64_t mido_lse_count;\n'
     'static struct { int pid; uint32_t n; char name[17]; } mido_lse_top[8];\n'
     'extern char *proc_name_address(void *p);\n'
     '\n'
     'static void\n'
     'mido_lse_account(void)\n'
     '{\n'
     '\tint pid = task_pid(current_task());\n'
     '\tunsigned slot = 0, min = 0;\n'
     '\tfor (slot = 0; slot < 8; slot++) {\n'
     '\t\tif (mido_lse_top[slot].pid == pid && mido_lse_top[slot].n) {\n'
     '\t\t\tmido_lse_top[slot].n++;\n'
     '\t\t\treturn;\n'
     '\t\t}\n'
     '\t\tif (mido_lse_top[slot].n < mido_lse_top[min].n) {\n'
     '\t\t\tmin = slot;\n'
     '\t\t}\n'
     '\t}\n'
     '\tif (mido_lse_top[min].n > 1) {\n'
     '\t\tmido_lse_top[min].n--;                   /* decay: keep the hot ones */\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tvoid *p = get_bsdtask_info(current_task());\n'
     '\tconst char *nm = p ? proc_name_address(p) : "?";\n'
     '\tmido_lse_top[min].pid = pid;\n'
     '\tmido_lse_top[min].n = 1;\n'
     '\tstrlcpy(mido_lse_top[min].name, nm ? nm : "?", sizeof(mido_lse_top[min].name));\n'
     '}\n'
     '\n'
     '/* Called once a minute from the startup.c heartbeat. */\n'
     'void mido_lse_report(void);\n'
     'void\n'
     'mido_lse_report(void)\n'
     '{\n'
     '\tstatic uint64_t last;\n'
     '\tuint64_t now = mido_lse_count;\n'
     '\tprintf("MIDO LSE: %llu emulated (+%llu in the last minute)", now, now - last);\n'
     '\tfor (int r = 0; r < 3; r++) {\n'
     '\t\tint best = -1;\n'
     '\t\tfor (int i = 0; i < 8; i++) {\n'
     '\t\t\tif (mido_lse_top[i].n && (best < 0 || mido_lse_top[i].n > mido_lse_top[best].n)) {\n'
     '\t\t\t\tbest = i;\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tif (best < 0) {\n'
     '\t\t\tbreak;\n'
     '\t\t}\n'
     '\t\tprintf(" %s[%d]:%u", mido_lse_top[best].name, mido_lse_top[best].pid, mido_lse_top[best].n);\n'
     '\t\tmido_lse_top[best].n = 0;\n'
     '\t}\n'
     '\tprintf("\\n");\n'
     '\tbzero(mido_lse_top, sizeof(mido_lse_top));\n'
     '\tlast = now;\n'
     '}\n')
edit('osfmk/arm64/sleh.c',
     '\t\t\tif (mido_lse_count++ == 0) {\n',
     '\t\t\tmido_lse_account();\n'
     '\t\t\tif (mido_lse_count++ == 0) {\n')
edit('osfmk/kern/startup.c',
     '\tif (stage != last_stage || (mido_ticks % 12) == 0) {\n'
     '\t\tprintf("MIDO tick %u: bootstrap at %s\\n", mido_ticks, stage);\n'
     '\t}\n',
     '\tif (stage != last_stage || (mido_ticks % 12) == 0) {\n'
     '\t\tprintf("MIDO tick %u: bootstrap at %s\\n", mido_ticks, stage);\n'
     '\t}\n'
     '\tif ((mido_ticks % 12) == 0) {\n'
     '\t\textern void mido_lse_report(void);\n'
     '\t\tmido_lse_report();\n'
     '\t}\n')

# ---------------------------------------------------------------------------
# GPU step 2: MidoGPU IOService + user client (buffer objects, submit, fences) on top
# of bsd/dev/mido_gpu.c. Matches IOResources like the other built-in SWEET services.
# ---------------------------------------------------------------------------
create('iokit/Kernel/arm/MidoGPU.cpp',
       (Path(__file__).resolve().parent / 'src' / 'MidoGPU.cpp').read_text())
edit('iokit/conf/files.arm64',
     'iokit/Kernel/arm/hid/IOHIDEventServiceUserClient.cpp             optional iokitcpp\n',
     'iokit/Kernel/arm/hid/IOHIDEventServiceUserClient.cpp             optional iokitcpp\n'
     'iokit/Kernel/arm/MidoGPU.cpp                                     optional iokitcpp\n')
edit('iokit/KernelConfigTables.cpp',
     "    \"     'IOMatchCategory' = IOWatchdog;\"\n"
     "    \"   },\"\n",
     "    \"     'IOMatchCategory' = IOWatchdog;\"\n"
     "    \"   },\"\n"
     '#if defined(MIDO_BOARD)\n'
     "    \"   {\"\n"
     "    \"     'IOClass'         = MidoGPU;\"\n"
     "    \"     'IOProviderClass' = IOResources;\"\n"
     "    \"     'IOMatchCategory' = MidoGPU;\"\n"
     "    \"   },\"\n"
     '#endif\n')

if failures:
    print('MIDO EDIT FAILURES:')
    for f in failures:
        print('  ' + f)
    sys.exit(1)
print('mido_xnu.py: all edits applied')

