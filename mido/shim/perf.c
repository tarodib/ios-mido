/*
 * Read-only performance probe (every boot, ~100 ms): the boot CPU's real clock
 * (PMU cycle counter against the 19.2 MHz CNTPCT), plus the MSM8953 CPU PLL and
 * the cluster / CCI muxes as LK left them (mido.dtb qcom,cpu-clock-8953:
 * c0-pll 0x0b116000, c0-mux 0x0b111050, c1-mux 0x0b011050, cci-mux 0x0b1d1050).
 * ios-sailfish found its bootloader left CPUs on the 19.2 MHz XO; check ours.
 */
#include <stdint.h>
#include "console.h"
#include "perf.h"

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static uint64_t cntpct(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t cycles(void) { uint64_t v; __asm__ volatile("isb; mrs %0, pmccntr_el0" : "=r"(v)); return v; }

unsigned perf_probe(void)
{
	uint64_t pmcr;
	__asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile("msr pmccfiltr_el0, xzr\n\t"           /* count at all ELs */
	                 "msr pmcntenset_el0, %0\n\t"
	                 "msr pmcr_el0, %1\n\tisb"
	                 :: "r"(1ULL << 31), "r"(pmcr | 1 | 4) : "memory");   /* E, reset CCNT */
	uint64_t t0 = cntpct(), c0 = cycles();
	while (cntpct() - t0 < 1920000)                              /* 100 ms */
		;
	uint64_t c1 = cycles(), t1 = cntpct();
	unsigned mhz = (unsigned)((c1 - c0) * 19200000ULL / ((t1 - t0) * 1000000ULL));

	con_puts("PERF: CPU0 "); con_dec(mhz); con_puts(" MHZ\n");
	con_puts(" C0MUX="); con_hex(rd(0x0b111050), 8); con_hex(rd(0x0b111054), 8);
	con_puts(" C1MUX="); con_hex(rd(0x0b011050), 8); con_hex(rd(0x0b011054), 8);
	con_puts("\n CCIMUX="); con_hex(rd(0x0b1d1050), 8); con_hex(rd(0x0b1d1054), 8);
	con_puts(" PLL:");
	for (unsigned i = 0; i < 0x30; i += 4) {
		con_putc(' '); con_hex(rd(0x0b116000 + i), 8);
	}
	con_putc('\n');
	return mhz;
}

/*
 * Run 1 (2026-11-06): CPU0 799 MHz. C0/C1 mux CFG 0x401 = source 4 (GPLL0 800 MHz)
 * /1, CCI 0x403 = GPLL0/2; the CPU PLL is off (MODE/L = 0; USER_CTL 0xf, CONFIG_CTL
 * 0x200d4828 / 0x006 = Linux clock-cpu-8953 apcs_cpu_pll defaults).
 * apcs_cpu_pll (Linux clock-cpu-8953.c): MODE +0x00 (b0 OUTCTRL, b1 BYPASSNL,
 * b2 RESET_N, b31 LOCK), L_VAL +0x08, ALPHA +0x10, USER_CTL +0x18, CONFIG_CTL
 * +0x20/+0x24, TEST_CTL +0x30/+0x34; rate = 19.2 MHz * L (min 652.8, max 2208 MHz).
 * Cluster mux (rcg mux-div): CMD +0 (b0 UPDATE), CFG +4: SRC_SEL[10:8] (5 = PLL,
 * 4 = GPLL0, 0 = XO), SRC_DIV[4:0] half-integer (1 = /1).
 */
#define CPU_PLL   0x0b116000UL
#define C0_MUX    0x0b111050UL
#define C1_MUX    0x0b011050UL

static inline void wrr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void udelay_ticks(uint64_t t) { uint64_t e = cntpct() + t; while (cntpct() < e) ; }

static int mux_set(uintptr_t mux, uint32_t src, uint32_t div)
{
	wrr(mux + 4, (src << 8) | div);
	wrr(mux, rd(mux) | 1u);                                      /* UPDATE */
	for (int i = 0; i < 100000; i++)
		if (!(rd(mux) & 1u))
			return 0;
	return -1;
}

/* Bring up the CPU PLL at 19.2 MHz * l and move both clusters onto it. */
int perf_cpu_pll(unsigned l)
{
	/* both clusters to GPLL0 /1 (where LK had them) while the PLL is (re)programmed */
	if (rd(CPU_PLL + 0x00) & 1u) {
		mux_set(C0_MUX, 4, 1);
		mux_set(C1_MUX, 4, 1);
		udelay_ticks(200);
	}
	wrr(CPU_PLL + 0x00, 0);
	wrr(CPU_PLL + 0x08, l);
	wrr(CPU_PLL + 0x10, 0);
	wrr(CPU_PLL + 0x20, 0x200d4828u);
	wrr(CPU_PLL + 0x24, 0x00000006u);
	wrr(CPU_PLL + 0x34, 0x00004000u);
	wrr(CPU_PLL + 0x30, 0x1c000000u);
	wrr(CPU_PLL + 0x18, rd(CPU_PLL + 0x18) | 0x9u);               /* main + early outputs */
	__asm__ volatile("dsb sy");
	wrr(CPU_PLL + 0x00, 2u);                                     /* BYPASSNL */
	udelay_ticks(200);                                           /* ~10 us */
	wrr(CPU_PLL + 0x00, 6u);                                     /* + RESET_N */
	int locked = 0;
	for (int i = 0; i < 2000; i++) {                             /* up to ~1 ms */
		if (rd(CPU_PLL + 0x00) & (1u << 31)) {
			locked = 1;
			break;
		}
		udelay_ticks(10);
	}
	con_puts("PERF: PLL L="); con_dec(l); con_puts(" MODE="); con_hex(rd(CPU_PLL), 8);
	if (!locked) {
		con_puts(" NO LOCK - PLL OFF, STAYING ON GPLL0\n");
		wrr(CPU_PLL + 0x00, 0);
		return -2;
	}
	wrr(CPU_PLL + 0x00, 7u);                                     /* + OUTCTRL */
	udelay_ticks(200);
	int e0 = mux_set(C0_MUX, 5, 1);
	int e1 = mux_set(C1_MUX, 5, 1);
	con_puts(" LOCKED, MUX E="); con_dec((uint64_t)(int64_t)e0); con_puts("/"); con_dec((uint64_t)(int64_t)e1);
	con_putc('\n');
	return e0 | e1;
}

/* PM8953 S5 (APC rail, SID 1 PPID 0x120): raw status / voltage registers, read only. */
void perf_apc_diag(int (*spmi_rd)(int, uint8_t, uint8_t *), int (*find)(uint32_t))
{
	int ch = find(0x120);
	uint8_t st = 0, en = 0, r0 = 0, v0 = 0, v1 = 0, mode = 0;
	if (ch >= 0) {
		spmi_rd(ch, 0x08, &st);
		spmi_rd(ch, 0x40, &r0);
		spmi_rd(ch, 0x41, &v0);
		spmi_rd(ch, 0x42, &v1);
		spmi_rd(ch, 0x45, &mode);
		spmi_rd(ch, 0x46, &en);
	}
	con_puts("PERF: APC S5 CH="); con_dec((uint64_t)(int64_t)ch);
	con_puts(" ST="); con_hex(st, 2); con_puts(" EN="); con_hex(en, 2);
	con_puts(" V40="); con_hex(r0, 2); con_puts(" V41="); con_hex(v0, 2); con_puts(" V42="); con_hex(v1, 2);
	con_puts(" MODE="); con_hex(mode, 2); con_putc('\n');
}

/* Deterministic integer + memory workload; a mismatch = the CPU is unstable. */
static uint64_t perf_work(void)
{
	static volatile uint64_t buf[1024];
	uint64_t h = 0x9e3779b97f4a7c15ULL;
	for (unsigned r = 0; r < 400; r++) {
		for (unsigned i = 0; i < 1024; i++) {
			h ^= h << 13; h ^= h >> 7; h ^= h << 17;
			buf[i] = h + i * r;
		}
		for (unsigned i = 0; i < 1024; i += 3)
			h = h * 0x100000001b3ULL ^ buf[(i * 7 + r) & 1023];
	}
	return h;
}

/*
 * "Crank it up to max": step the CPU PLL up through the msm8953 table at LK's APC
 * voltage, checking a workload checksum at each step; on a mismatch go back to
 * the last good L. A hang leaves "TRY L=.." as the last line. Returns the final L.
 */
unsigned perf_sweep(void)
{
	static const unsigned ls[] = {73, 88, 94, 102, 105};   /* 1401.6 1689.6 1804.8 1958.4 2016 */
	uint64_t ref = perf_work();
	unsigned good = 0, last = sizeof(ls) / sizeof(ls[0]) - 1;
	for (unsigned i = 0; i <= last; i++) {
		con_puts("PERF: TRY L="); con_dec(ls[i]); con_putc('\n');
		if (perf_cpu_pll(ls[i]))
			break;
		uint64_t t0 = cntpct(), c0 = cycles();
		while (cntpct() - t0 < 960000)                       /* 50 ms */
			;
		unsigned mhz = (unsigned)((cycles() - c0) * 19200000ULL / ((cntpct() - t0) * 1000000ULL));
		int ok = 1;
		for (int k = 0; k < 3 && ok; k++)
			ok = (perf_work() == ref);
		con_puts("PERF: L="); con_dec(ls[i]); con_putc(' '); con_dec(mhz);
		con_puts(ok ? " MHZ OK\n" : " MHZ BAD\n");
		if (!ok)
			break;
		good = ls[i];
	}
	if (good && good != ls[last])
		perf_cpu_pll(good);                                  /* back to the last good one */
	con_puts("PERF: SWEEP DONE, L="); con_dec(good); con_putc('\n');
	return good;
}

/*
 * Every XNU boot: the sweep (run 3: 1401..2015 MHz all OK at LK's S5 0x9e, ~0.87 V;
 * the highest passing step stays) and the CCI from GPLL0/2 (400) to GPLL0/1 (800 MHz,
 * table max 806.4).
 */
void perf_boost(void)
{
	unsigned l = perf_sweep();
	int e = mux_set(0x0b1d1050UL, 4, 1);
	con_puts("PERF: BOOST CPU L="); con_dec(l); con_puts(" ("); con_dec(l * 192 / 10);
	con_puts(" MHZ) CCI 800 MHZ E="); con_dec((uint64_t)(int64_t)e); con_putc('\n');
}
