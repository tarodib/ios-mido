/*
 * mido shim, milestone M1.5 — one boot answers the questions that shape the port:
 *   CPU: exception level, 16K granule support, LSE atomics, PAR range
 *   PSCI: version, CPU_ON for all 7 secondaries (MPIDR 0x0-0x3, 0x100-0x103)
 *   GICv2 (0x0B000000 / 0x0B002000): virtual timer PPI (INTID 20) delivery
 *   SPMI/PMIC: PON channel lookup + reads, then a WARM reset with
 *              restart_reason=bootloader so the phone lands in fastboot by itself
 *   Every line is also kept at 0x9FF00000 (pstore RAM) for later retrieval.
 */
#include <stdint.h>
#include "console.h"
#include "sdhc.h"
#include "spmi.h"

#define IMEM_RESTART_REASON 0x0860065cUL
#define RESTART_BOOTLOADER  0x77665500U
#define PSHOLD              0x004ab000UL

#define GICD 0x0b000000UL
#define GICC 0x0b002000UL
#define VTIMER_INTID 20

#define PON_PPID    0x008          /* SID 0, peripheral 0x08 (PON @ 0x800) */
#define PON_REVISION2        0x01
#define PON_PS_HOLD_RST_CTL  0x5a
#define PON_PS_HOLD_RST_CTL2 0x5b
#define PON_SOFT_RB_SPARE    0x8f
#define PON_RESET_WARM       0x1

#define PSCI_VERSION      0x84000000UL
#define PSCI_CPU_OFF      0x84000002UL
#define PSCI_CPU_ON       0xc4000003UL
#define PSCI_SYSTEM_RESET 0x84000009UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];
static volatile uint64_t sec_mpidr[8];
static volatile uint32_t irq_count, irq_other;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			 "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + 19200ULL * ms;
	while (now() < end)
		;
}

/* ---------------- SPMI / PON (driver in spmi.c) ---------------- */

static int pon_chan = -1;

static int pon_read(uint8_t reg, uint8_t *val) { return spmi_read(pon_chan, reg, val); }
static int pon_write(uint8_t reg, uint8_t val) { return spmi_write(pon_chan, reg, val); }

static void __attribute__((noreturn)) warm_reset_to_fastboot(void)
{
	int ok = 0;
	if (pon_chan >= 0) {
		uint8_t ctl;
		ok = pon_write(PON_PS_HOLD_RST_CTL2, 0x00) == 0;     /* disable while changing type */
		delay_ms(1);
		ok &= pon_read(PON_PS_HOLD_RST_CTL, &ctl) == 0;
		ok &= pon_write(PON_PS_HOLD_RST_CTL, (ctl & ~0xf) | PON_RESET_WARM) == 0;
		ok &= pon_write(PON_PS_HOLD_RST_CTL2, 0x80) == 0;    /* re-enable */
		/* LK (USE_PON_REBOOT_REG) reads reboot mode from SOFT_RB_SPARE[7:2], 2 = fastboot,
		 * and scrubs it after reading. */
		uint8_t sp;
		ok &= pon_read(PON_SOFT_RB_SPARE, &sp) == 0;
		ok &= pon_write(PON_SOFT_RB_SPARE, (sp & 0x03) | (0x02 << 2)) == 0;
	}
	con_puts(ok ? "WARM RESET ARMED, REBOOTING\n" : "PON WRITE FAILED, COLD RESET\n");
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(50);
	wr32(PSHOLD, 0);                     /* PMIC performs the configured reset */
	delay_ms(2000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);     /* fallback */
	for (;;)
		__asm__ volatile("wfe");
}

/* ---------------- exceptions ---------------- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {                            /* IRQ, current EL, SPx */
		uint32_t iar = rd32(GICC + 0x0c);
		uint32_t id = iar & 0x3ff;
		if (id == 1023)
			return;
		if (id == VTIMER_INTID) {
			irq_count++;
			wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);
		} else
			irq_other++;
		wr32(GICC + 0x10, iar);
		return;
	}
	con_clear(C_RED);
	con_puts("EXCEPTION SLOT "); con_dec(slot); con_putc('\n');
	con_kv("ESR", rsr("esr_el1"));
	con_kv("ELR", rsr("elr_el1"));
	con_kv("FAR", rsr("far_el1"));
	con_kv("X0", regs[0]);
	con_kv("LR", regs[20]);
	delay_ms(20000);
	warm_reset_to_fastboot();
}

/* ---------------- SMP ---------------- */

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx & 7] = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void test_smp(void)
{
	static const uint16_t mpidr[8] = {0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103};
	uint64_t self = rsr("mpidr_el1") & 0xffff;
	int64_t ret[8];
	for (int i = 1; i < 8; i++)
		ret[i] = (int64_t)smc(PSCI_CPU_ON, mpidr[i] == self ? mpidr[0] : mpidr[i],
				      (uintptr_t)secondary_entry, i);
	delay_ms(500);
	int up = 1;
	for (int i = 1; i < 8; i++) {
		con_puts(" CPU"); con_dec(i); con_puts(" RET=");
		con_dec((uint32_t)ret[i]);
		con_puts(" MPIDR=");
		if (sec_mpidr[i]) { con_hex(sec_mpidr[i] & 0xffffff, 6); up++; }
		else con_puts("NONE");
		con_putc('\n');
	}
	con_puts("CPUS ONLINE: "); con_dec(up); con_puts("/8\n");
}

/* ---------------- GICv2 + virtual timer ---------------- */

static void test_gic(void)
{
	con_kv("GICD TYPER", rd32(GICD + 0x04));
	con_kv("GICD IIDR", rd32(GICD + 0x08));
	con_kv("GICC IIDR", rd32(GICC + 0xfc));
	wr32(GICD + 0x000, rd32(GICD) | 1);
	wr8(GICD + 0x400 + VTIMER_INTID, 0x80);
	wr32(GICD + 0x100, 1U << VTIMER_INTID);
	wr32(GICC + 0x004, 0xf0);                 /* PMR */
	wr32(GICC + 0x000, rd32(GICC) | 1);       /* enable CPU interface */
	irq_count = irq_other = 0;
	wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);   /* 10 ms */
	wsr("cntv_ctl_el0", 1);
	__asm__ volatile("isb; msr daifclr, #2" ::: "memory");
	delay_ms(300);
	__asm__ volatile("msr daifset, #2; isb" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wr32(GICD + 0x180, 1U << VTIMER_INTID);
	con_puts("VTIMER IRQS IN 300MS: "); con_dec(irq_count);
	con_puts(" (EXPECT ~30) OTHER: "); con_dec(irq_other); con_putc('\n');
}

/* ---------------- eMMC + GPT + log to `cache` ---------------- */

/* Measured from Linux (OrangeFox): mmcblk0p25 "cache". Writes require an exact GPT match. */
#define CACHE_FIRST 7471104ULL
#define CACHE_SECTORS 524288ULL
#define LOG_SECTORS 128            /* 64 KB RAM log at LOG_PHYS */

static uint32_t blk[128];
static int log_ok;
/* Failure code, also signalled to the host by timing (hold = 5 s + 4 s * code):
 * 0 ok | 1 bus power stays off | 2 card never reached TRAN | 3 LBA0 read/sig bad
 * 4 LBA1 not GPT | 5 GPT entry read failed | 6 cache != expected | 7 no cache entry
 * 8 log write failed | 9 read-back verify failed
 * re-init: 10 CMD1 answered but busy forever | 11 CMD2 | 12 CMD3 | 13 CMD7 | 14 not TRAN after re-init
 *          16..20 CMD1 error (1 busy, 2 cmd timeout, 3 cmd error, 4/5 data) | 21 GCC CFG sanity fail
 *          22 GCC RCG update never latched */
static unsigned emmc_code;

static int name_is_cache(const uint32_t *e)
{
	const uint16_t *n = (const uint16_t *)((const uint8_t *)e + 56);
	return n[0] == 'c' && n[1] == 'a' && n[2] == 'c' && n[3] == 'h' && n[4] == 'e' && n[5] == 0;
}

static void test_emmc(void)
{
	int r = sdhc_init();
	if (r) {
		con_puts("EMMC INIT FAILED\n");
		emmc_code = r == -1 ? 1 : r <= -10 ? (unsigned)-r : 2;
		return;
	}
	int e = sdhc_read(0, blk);
	con_puts("LBA0 E="); con_dec(e); con_puts(" SIG="); con_hex(blk[127] >> 16, 4);
	con_puts(blk[127] >> 16 == 0xaa55 ? " OK\n" : " BAD\n");
	if (e || blk[127] >> 16 != 0xaa55) {
		emmc_code = 3;
		return;
	}
	e = sdhc_read(1, blk);
	int gpt = !e && blk[0] == 0x20494645 && blk[1] == 0x54524150;   /* "EFI PART" */
	con_puts("LBA1 E="); con_dec(e); con_puts(gpt ? " GPT OK\n" : " NO GPT\n");
	if (!gpt) {
		emmc_code = 4;
		return;
	}
	uint64_t ent_lba = blk[18] | (uint64_t)blk[19] << 32;
	uint32_t n = blk[20], esz = blk[21];
	if (esz != 128 || n > 128) {
		emmc_code = 5;
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && sdhc_read(ent_lba + i / 4, blk)) {
			emmc_code = 5;
			return;
		}
		const uint32_t *ent = blk + (i % 4) * 32;
		if (!name_is_cache(ent))
			continue;
		uint64_t first = ent[8] | (uint64_t)ent[9] << 32;
		uint64_t last = ent[10] | (uint64_t)ent[11] << 32;
		con_puts("GPT CACHE #"); con_dec(i + 1); con_puts(" FIRST="); con_dec(first);
		con_puts(" N="); con_dec(last - first + 1); con_putc('\n');
		if (first == CACHE_FIRST && last - first + 1 == CACHE_SECTORS) {
			sdhc_allow_writes(CACHE_FIRST, CACHE_FIRST + LOG_SECTORS);
			log_ok = 1;
			con_puts("MATCHES LINUX, LOG -> CACHE ENABLED\n");
		} else {
			con_puts("MISMATCH, WRITES DISABLED\n");
			emmc_code = 6;
		}
		return;
	}
	con_puts("NO CACHE PARTITION\n");
	emmc_code = 7;
}

static void flush_log(void)
{
	if (!log_ok)
		return;
	int e = 0;
	for (int i = 0; i < LOG_SECTORS && !e; i++)
		e = sdhc_write(CACHE_FIRST + i, (const void *)(con_log_phys() + 512UL * i));
	int ok = !e && !sdhc_read(CACHE_FIRST, blk) && blk[0] == *(volatile uint32_t *)con_log_phys();
	con_puts("LOG WRITE E="); con_dec(e); con_puts(ok ? " VERIFY OK\n" : " VERIFY FAIL\n");
	if (e)
		emmc_code = 8;
	else if (!ok)
		emmc_code = 9;
	if (!e)   /* rewrite first sectors so the result line is in the saved log too */
		for (int i = 0; i < 8 && !e; i++)
			e = sdhc_write(CACHE_FIRST + i, (const void *)(con_log_phys() + 512UL * i));
}

/* ---------------- main ---------------- */

void shim_main(uint64_t dtb)
{
	con_log_init();
	con_clear(C_NAVY);
	con_puts("MIDO SHIM M1.5\n\n");

	uint64_t el = (rsr("CurrentEL") >> 2) & 3;
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	uint64_t isar0 = rsr("id_aa64isar0_el1");
	con_puts("EL"); con_dec(el); con_putc('\n');
	con_kv("MIDR", rsr("midr_el1"));
	con_kv("MPIDR", rsr("mpidr_el1"));
	con_kv("MMFR0", mmfr0);
	con_kv("ISAR0", isar0);
	con_kv("PFR0", rsr("id_aa64pfr0_el1"));
	con_kv("CNTFRQ", rsr("cntfrq_el0"));
	con_puts("16K PAGES: "); con_puts(((mmfr0 >> 20) & 0xf) == 1 ? "YES\n" : "NO\n");
	con_puts("LSE ATOMICS: "); con_puts(((isar0 >> 20) & 0xf) >= 2 ? "YES\n" : "NO\n");
	con_kv("DTB", dtb);
	con_kv("DTB MAGIC", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_putc('\n');

	con_kv("PSCI VER", smc(PSCI_VERSION, 0, 0, 0));
	test_smp();
	con_putc('\n');

	test_gic();
	con_putc('\n');

	pon_chan = spmi_find_chan(PON_PPID);
	con_puts("SPMI PON CHAN: "); con_dec((uint64_t)(int64_t)pon_chan); con_putc('\n');
	if (pon_chan >= 0) {
		uint8_t v;
		int e = pon_read(PON_REVISION2, &v);
		con_puts(" PON REV2="); con_hex(v, 2); con_puts(" ERR="); con_dec(e); con_putc('\n');
		pon_read(PON_PS_HOLD_RST_CTL, &v);
		con_puts(" RST CTL="); con_hex(v, 2);
		pon_read(PON_PS_HOLD_RST_CTL2, &v);
		con_puts(" CTL2="); con_hex(v, 2);
		pon_read(PON_SOFT_RB_SPARE, &v);
		con_puts(" SPARE="); con_hex(v, 2); con_putc('\n');
	}
	con_putc('\n');
	test_emmc();
	con_putc('\n');
	flush_log();
	con_puts("EMMC CODE "); con_dec(emmc_code);
	con_puts(", HOLD "); con_dec(5 + 4 * emmc_code); con_puts("S, THEN FASTBOOT\n");
	delay_ms(1000 * (5 + 4 * emmc_code));
	warm_reset_to_fastboot();
}
