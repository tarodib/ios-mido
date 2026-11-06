/*
 * Minimal SDHCI-MSM eMMC driver for the mido shim (PIO, single block).
 * Controller: SDHC1 @ 0x07824900 (standard SDHCI regs), vendor core @ 0x07824000.
 *
 * LK's target_uninit() normally puts the card to sleep (CMD7 deselect + CMD5
 * sleep) and switches the controller back to legacy MCI mode before jumping
 * to the kernel. We therefore: re-enable HC mode, wake the card (CMD5 awake),
 * select it (CMD7) and keep LK's bus width / timing / clock untouched.
 *
 * Writes are only allowed inside a window set by sdhc_allow_writes(), which
 * the caller sets only after the on-chip GPT matched the expected partition.
 */
#include <stdint.h>
#include "console.h"
#include "sdhc.h"
#include "spmi.h"

static uint32_t pwr_irq_service(unsigned ms);

#define HC       0x07824900UL
#define CORE     0x07824000UL
#define CORE_HC_MODE 0x78

#define R_BLK    0x04   /* [15:0] block size, [31:16] block count */
#define R_ARG    0x08
#define R_XFER   0x0c   /* [15:0] transfer mode, [31:16] command */
#define R_RESP0  0x10
#define R_BUF    0x20
#define R_PRES   0x24
#define R_HCTL   0x28   /* host ctl, power ctl, blockgap, wakeup */
#define R_CLK    0x2c   /* [15:0] clock ctl, [23:16] timeout, [31:24] sw reset */
#define R_STS    0x30   /* [15:0] normal, [31:16] error */
#define R_STSEN  0x34
#define R_SIGEN  0x38
#define R_HCTL2  0x3c
#define R_CAPS   0x40

#define ST_CMD_DONE  (1U << 0)
#define ST_XFER_DONE (1U << 1)
#define ST_WR_READY  (1U << 4)
#define ST_RD_READY  (1U << 5)
#define ST_ERR       (1U << 15)

#define RSP_NONE 0x00
#define RSP_R1   0x1a   /* 48-bit, CRC + index check */
#define RSP_R1B  0x1b   /* 48-bit busy */

static inline uint32_t rd(uintptr_t o) { return *(volatile uint32_t *)(HC + o); }
static inline void wr(uintptr_t o, uint32_t v) { *(volatile uint32_t *)(HC + o) = v; }

static uint32_t rca;
static uint32_t last_resp;
static uint32_t last_err;     /* SDHCI error status of the last failed command */
static int force_reinit = 1;   /* run 3: wake path failed (code 2) -> go straight to re-init */
static int full_reinit(void);
static uint64_t wr_lo, wr_hi;   /* allowed write window [lo, hi) */

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

/* wait for any of `mask` (or error) in normal status, up to ms */
static uint32_t wait_sts(uint32_t mask, unsigned ms)
{
	uint64_t end = ticks() + 19200ULL * ms;
	uint32_t s;
	do {
		s = rd(R_STS);
		if (s & (mask | ST_ERR))
			return s;
	} while (ticks() < end);
	return 0;   /* timeout */
}

static void reset_lines(void)
{
	wr(R_CLK, rd(R_CLK) | (0x06U << 24));   /* reset CMD + DAT lines */
	uint64_t end = ticks() + 19200ULL * 100;
	while ((rd(R_CLK) >> 24) & 0x06 && ticks() < end)
		;
	wr(R_STS, 0xffffffff);
}

/* returns 0 ok, else error code: 1 busy, 2 cmd timeout, 3 cmd error, 4 data timeout, 5 data error */
static int cmd(uint32_t idx, uint32_t arg, uint32_t rsp, int data_dir, uint32_t *buf)
{
	uint64_t end = ticks() + 19200ULL * 500;
	while (rd(R_PRES) & (data_dir || rsp == RSP_R1B ? 3 : 1))
		if (ticks() > end)
			return 1;
	wr(R_STS, 0xffffffff);
	uint32_t xfer = 0, c = (idx << 8) | rsp;
	if (data_dir) {
		wr(R_BLK, (1U << 16) | 512);
		xfer = (1U << 1) | (data_dir < 0 ? (1U << 4) : 0);   /* blkcnt en, read dir */
		c |= 0x20;                                            /* data present */
	}
	wr(R_ARG, arg);
	wr(R_XFER, (c << 16) | xfer);

	uint32_t s = wait_sts(ST_CMD_DONE, 1000);
	if (!s) { reset_lines(); return 2; }
	if (s & ST_ERR) {
		last_err = s >> 16;
		reset_lines();
		return (last_err & 1) ? 2 : 3;   /* error bit0 = command timeout (no response) */
	}
	last_resp = rd(R_RESP0);
	wr(R_STS, ST_CMD_DONE);

	if (data_dir < 0) {
		s = wait_sts(ST_RD_READY, 1000);
		if (!s || (s & ST_ERR)) { reset_lines(); return s ? 5 : 4; }
		wr(R_STS, ST_RD_READY);
		for (int i = 0; i < 128; i++)
			buf[i] = rd(R_BUF);
	} else if (data_dir > 0) {
		s = wait_sts(ST_WR_READY, 1000);
		if (!s || (s & ST_ERR)) { reset_lines(); return s ? 5 : 4; }
		wr(R_STS, ST_WR_READY);
		for (int i = 0; i < 128; i++)
			wr(R_BUF, buf[i]);
	}
	if (data_dir || rsp == RSP_R1B) {
		s = wait_sts(ST_XFER_DONE, 2000);
		if (!s || (s & ST_ERR)) { reset_lines(); return s ? 5 : 4; }
	}
	wr(R_STS, 0xffffffff);
	return 0;
}

/* MSM vendor power-request handshake (sdhci-msm v4 core offsets). */
#define CORE_PWRCTL_STATUS 0xdc
#define CORE_PWRCTL_MASK   0xe0
#define CORE_PWRCTL_CLEAR  0xe4
#define CORE_PWRCTL_CTL    0xe8
#define REQ_BUS_OFF 0x1
#define REQ_BUS_ON  0x2
#define REQ_IO_LOW  0x4
#define REQ_IO_HIGH 0x8
#define ACK_BUS_OK  0x1
#define ACK_IO_OK   0x4

static inline uint32_t crd(uintptr_t o) { return *(volatile uint32_t *)(CORE + o); }
static inline void cwr(uintptr_t o, uint32_t v) { *(volatile uint32_t *)(CORE + o) = v; }

/* Ack every power request raised within `ms` (regulators are already on from LK). */
static uint32_t pwr_irq_service(unsigned ms)
{
	uint32_t seen = 0;
	uint64_t end = ticks() + 19200ULL * ms;
	cwr(CORE_PWRCTL_MASK, 0xff);
	while (ticks() < end) {
		uint32_t st = crd(CORE_PWRCTL_STATUS) & 0xf;
		if (!st)
			continue;
		seen |= st;
		cwr(CORE_PWRCTL_CLEAR, st);
		uint32_t ack = 0;
		if (st & (REQ_BUS_ON | REQ_BUS_OFF)) ack |= ACK_BUS_OK;
		if (st & (REQ_IO_LOW | REQ_IO_HIGH)) ack |= ACK_IO_OK;
		cwr(CORE_PWRCTL_CTL, ack);
	}
	return seen;
}

static int card_state(void)
{
	int e = cmd(13, rca << 16, RSP_R1, 0, 0);
	return e ? -e : (int)((last_resp >> 9) & 0xf);
}

/* PM8953 LDOs on SID 1: L5 = vdd-io (1.8 V), L8 = vdd (2.9 V). Read-only status dump. */
static void regulator_diag(void)
{
	static const uint16_t ppid[2] = {0x144, 0x147};
	for (int i = 0; i < 2; i++) {
		int ch = spmi_find_chan(ppid[i]);
		uint8_t st = 0, en = 0, v1 = 0, v2 = 0, mode = 0;
		if (ch >= 0) {
			spmi_read(ch, 0x08, &st);
			spmi_read(ch, 0x46, &en);
			spmi_read(ch, 0x40, &v1);
			spmi_read(ch, 0x41, &v2);
			spmi_read(ch, 0x45, &mode);
		}
		con_puts(i ? " L8" : " L5"); con_puts(" CH="); con_dec((uint64_t)(int64_t)ch);
		con_puts(" ST="); con_hex(st, 2); con_puts(" EN="); con_hex(en, 2);
		con_puts(" V="); con_hex(v1, 2); con_hex(v2, 2); con_puts(" M="); con_hex(mode, 2); con_putc('\n');
	}
}

/* TLMM SDC1 pad control (msm8953 0x0110A000): restore the DT "active" config:
 * clk no-pull 16 mA, cmd/data pull-up 10 mA, rclk pull-down. */
#define TLMM_SDC1_CTL 0x0110a000UL
static void pads_active(void)
{
	uint32_t v = *(volatile uint32_t *)TLMM_SDC1_CTL;
	con_puts(" SDC1 PADS="); con_hex(v, 8);
	v = (v & ~0x1ffffU) | (1U << 15) | (0U << 13) | (7U << 6) | (3U << 11) | (4U << 3) | (3U << 9) | 4U;
	*(volatile uint32_t *)TLMM_SDC1_CTL = v;
	con_puts("->"); con_hex(*(volatile uint32_t *)TLMM_SDC1_CTL, 8); con_putc('\n');
}

/* As Linux sdhci-msm probe: core SW reset, HC mode + FF_CLK_SW_RST_DIS, SDHCI reset-all. */
#define CORE_POWER 0x00
static void core_reset(void)
{
	volatile uint32_t *pw = (volatile uint32_t *)(CORE + CORE_POWER);
	*pw |= 1U << 7;
	uint64_t end = ticks() + 19200ULL * 10;
	while ((*pw & (1U << 7)) && ticks() < end)
		;
	con_puts(" CORE RST "); con_puts(*pw & (1U << 7) ? "STUCK" : "OK");
	*(volatile uint32_t *)(CORE + CORE_HC_MODE) = 1U | (1U << 13);
	wr(R_CLK, rd(R_CLK) | (1U << 24));                 /* SDHCI RESET_ALL */
	end = ticks() + 19200ULL * 100;
	while ((rd(R_CLK) & (1U << 24)) && ticks() < end)
		;
	con_puts(" HC RST "); con_puts(rd(R_CLK) & (1U << 24) ? "STUCK" : "OK");
	con_puts(" PWRREQ="); con_hex(pwr_irq_service(20), 2); con_putc('\n');
}

int sdhc_init(void)
{
	regulator_diag();
	pads_active();
	if (force_reinit)
		core_reset();
	uint32_t mode = *(volatile uint32_t *)(CORE + CORE_HC_MODE);
	con_puts("EMMC HC_MODE="); con_hex(mode, 8);
	if (!(mode & 1)) {
		*(volatile uint32_t *)(CORE + CORE_HC_MODE) = mode | 1;
		for (volatile int i = 0; i < 100000; i++)
			;
		con_puts("->"); con_hex(*(volatile uint32_t *)(CORE + CORE_HC_MODE), 8);
	}
	con_putc('\n');
	con_puts(" PRES="); con_hex(rd(R_PRES), 8);
	con_puts(" HCTL="); con_hex(rd(R_HCTL), 8); con_putc('\n');
	con_puts(" CLK="); con_hex(rd(R_CLK), 8);
	con_puts(" HCTL2="); con_hex(rd(R_HCTL2), 8); con_putc('\n');

	if (!(rd(R_HCTL) & (1U << 8))) {
		/* Bus power on (3.0V select | ON). On MSM this raises a power request
		 * in the vendor block that must be acked before it takes effect. */
		wr(R_HCTL, (rd(R_HCTL) & ~0xff00U) | (0x0dU << 8));
		uint32_t seen = pwr_irq_service(100);
		con_puts(" BUS POWER ON: PWRREQ="); con_hex(seen, 2);
		con_puts(" HCTL="); con_hex(rd(R_HCTL), 8); con_putc('\n');
		if (!(rd(R_HCTL) & (1U << 8))) {
			con_puts(" BUS POWER STILL OFF - ABORT\n");
			return -1;
		}
	}
	uint32_t clk = rd(R_CLK) & 0xffff;
	if ((clk & 0x7) != 0x7) {          /* internal clk en, stable, sd clk en */
		wr(R_CLK, (rd(R_CLK) & ~0xffffU) | clk | 1);
		uint64_t end = ticks() + 19200ULL * 20;
		while (!(rd(R_CLK) & 2) && ticks() < end)
			;
		wr(R_CLK, rd(R_CLK) | 4);
		con_puts(" CLK FIXED="); con_hex(rd(R_CLK), 8); con_putc('\n');
	}
	wr(R_STSEN, 0x03ff003f);
	wr(R_SIGEN, 0);

	static const uint32_t rcas[] = {2, 1};
	for (int i = 0; i < 2 && !force_reinit; i++) {
		rca = rcas[i];
		int st = card_state();
		con_puts(" RCA "); con_dec(rca); con_puts(" CMD13 STATE="); con_dec(st < 0 ? 100 - st : st);
		if (st < 0) {                                      /* asleep: CMD5 awake */
			int e = cmd(5, rca << 16, RSP_R1B, 0, 0);
			con_puts(" CMD5 E="); con_dec(e);
			st = card_state();
			con_puts(" ST="); con_dec(st < 0 ? 100 - st : st);
		}
		if (st == 3) {                                     /* stby -> tran */
			int e = cmd(7, rca << 16, RSP_R1B, 0, 0);
			con_puts(" CMD7 E="); con_dec(e);
			st = card_state();
			con_puts(" ST="); con_dec(st < 0 ? 100 - st : st);
		}
		con_putc('\n');
		if (st == 4)
			return 0;
	}
	return full_reinit();
}

/* ---- full re-identification at safe speed (legacy timing, 1-bit bus) ---- */

#define RSP_R2 0x09   /* 136-bit, CRC */
#define RSP_R3 0x02   /* 48-bit, no CRC/index check (OCR) */

static unsigned base_mhz;

static void set_clock_div(unsigned div)   /* SD clk = base / (2*div), div 1..1023 */
{
	uint32_t c = rd(R_CLK);
	wr(R_CLK, c & ~4U);                               /* SD clock off */
	c = (c & ~0xffc4U) | ((div & 0xff) << 8) | (((div >> 8) & 3) << 6) | 1;
	wr(R_CLK, c);
	uint64_t end = ticks() + 19200ULL * 20;
	while (!(rd(R_CLK) & 2) && ticks() < end)
		;
	wr(R_CLK, rd(R_CLK) | 4);                         /* SD clock on */
	for (volatile int i = 0; i < 20000; i++)
		;
}

/* ---- GCC SDCC1 apps clock (msm8953: RCG @ 0x01842004, branch @ 0x01842018) ---- */
#define GCC_SDCC1_CMD  0x01842004UL
#define GCC_SDCC1_CFG  0x01842008UL
#define GCC_SDCC1_M    0x0184200cUL
#define GCC_SDCC1_N    0x01842010UL
#define GCC_SDCC1_D    0x01842014UL
#define GCC_SDCC1_CBCR 0x01842018UL
/* NB: in sdhci-msm (3.18, MCI present) these two are offsets from the SDHCI base (HC),
 * not from the core block. Run 5 wrote them at CORE+ by mistake (read back 0). */
#define HC_DLL_CONFIG  0x100
#define HC_VENDOR_SPEC 0x10c
#define VS_IO_PAD_PWR_SWITCH_EN (1U << 15)
#define VS_IO_PAD_PWR_SWITCH    (1U << 16)   /* 1 = pads in 1.8 V mode */

static inline uint32_t g(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void gw(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* rate = src / div_x2*2 * m/n ; cfg built as clk-rcg2 does (dual-edge MND when m != n) */
static int gcc_sdcc1_set(unsigned src_sel, unsigned div_x2_minus1, unsigned m, unsigned n)
{
	uint32_t cfg = (src_sel << 8) | div_x2_minus1 | (m != n ? (2U << 12) : 0);
	if (m != n) {
		unsigned d = n;
		if (d > 2 * (n - m)) d = 2 * (n - m);
		gw(GCC_SDCC1_M, m);
		gw(GCC_SDCC1_N, ~(n - m) & 0xff);
		gw(GCC_SDCC1_D, ~d & 0xff);
	}
	gw(GCC_SDCC1_CFG, cfg);
	gw(GCC_SDCC1_CMD, g(GCC_SDCC1_CMD) | 1);          /* UPDATE */
	uint64_t end = ticks() + 19200ULL * 10;
	while ((g(GCC_SDCC1_CMD) & 1) && ticks() < end)
		;
	return g(GCC_SDCC1_CMD) & 1;                      /* nonzero = update never latched */
}

static int full_reinit(void)
{
	uint32_t caps = rd(R_CAPS);
	base_mhz = (caps >> 8) & 0xff;
	con_puts(" REINIT CAPS="); con_hex(caps, 8); con_putc('\n');
	con_puts(" GCC CMD="); con_hex(g(GCC_SDCC1_CMD), 8); con_puts(" CFG="); con_hex(g(GCC_SDCC1_CFG), 8);
	con_puts(" CBCR="); con_hex(g(GCC_SDCC1_CBCR), 8); con_putc('\n');
	/* Sanity: CFG must look like an RCG config (only src_sel/div/mode bits) */
	if (g(GCC_SDCC1_CFG) & ~0x371fU)
		return -21;

	wr(R_CLK, rd(R_CLK) & ~4U);                        /* SD clock off while switching */
	/* Undo HS400/HS200 routing (MCLK_SEL default, SELECT_IN off), pads to 1.8 V mode
	 * (VCCQ = L5 1.8 V), DLL reset + power-down (not needed below 100 MHz). */
	uint32_t vs = rd(HC_VENDOR_SPEC);
	con_puts(" VS WAS="); con_hex(vs, 8);
	wr(HC_VENDOR_SPEC, (vs & ~((3U << 8) | (0xfU << 18))) | (2U << 8) |
	   VS_IO_PAD_PWR_SWITCH_EN | VS_IO_PAD_PWR_SWITCH);
	wr(HC_DLL_CONFIG, rd(HC_DLL_CONFIG) | (1U << 30) | (1U << 29));
	con_puts(" NOW="); con_hex(rd(HC_VENDOR_SPEC), 8); con_putc('\n');
	/* UHS mode select = legacy, 1.8 V signaling enable (raises IO_LOW power request) */
	wr(R_HCTL2, (rd(R_HCTL2) & ~(0x7U << 16)) | (1U << 19));
	con_puts(" IO 1V8 PWRREQ="); con_hex(pwr_irq_service(20), 2); con_putc('\n');
	wr(R_HCTL, rd(R_HCTL) & ~((1U << 1) | (1U << 2) | (1U << 5)));  /* 1-bit, no HS */
	if (gcc_sdcc1_set(0, 23, 1, 4))                    /* XO 19.2M /12 * 1/4 = 400 kHz */
		return -22;
	gw(GCC_SDCC1_CBCR, g(GCC_SDCC1_CBCR) | 1);
	set_clock_div(0);                                  /* SDHCI divider bypass: base clock */
	reset_lines();
	con_puts(" AFTER: GCC CMD="); con_hex(g(GCC_SDCC1_CMD), 8); con_puts(" CFG="); con_hex(g(GCC_SDCC1_CFG), 8);
	con_putc('\n');
	con_puts("  CBCR="); con_hex(g(GCC_SDCC1_CBCR), 8); con_puts(" CLK="); con_hex(rd(R_CLK), 8);
	con_puts(" PRES="); con_hex(rd(R_PRES), 8); con_putc('\n');
	con_puts("  HCTL="); con_hex(rd(R_HCTL), 8); con_puts(" HCTL2="); con_hex(rd(R_HCTL2), 8);
	con_puts(" VS="); con_hex(rd(HC_VENDOR_SPEC), 8); con_putc('\n');

	cmd(0, 0, RSP_NONE, 0, 0);                         /* GO_IDLE (also exits sleep) */
	for (volatile int i = 0; i < 200000; i++)
		;
	uint64_t end = ticks() + 19200ULL * 1500;
	int e;
	do {
		e = cmd(1, 0x40ff8080, RSP_R3, 0, 0);          /* sector mode, 1.7-3.6 V */
	} while ((e || !(last_resp & (1U << 31))) && ticks() < end);
	con_puts(" CMD1 E="); con_dec(e); con_puts(" OCR="); con_hex(last_resp, 8);
	con_puts(" ERRSTS="); con_hex(last_err, 4); con_putc('\n');
	if (e)
		return -(15 + e);                              /* 16..20: CMD1 error type */
	if (!(last_resp & (1U << 31)))
		return -10;                                    /* answered, but busy forever */
	if (cmd(2, 0, RSP_R2, 0, 0))
		return -11;
	rca = 2;
	if (cmd(3, rca << 16, RSP_R1, 0, 0))
		return -12;
	if (cmd(7, rca << 16, RSP_R1B, 0, 0))
		return -13;
	wr(R_CLK, rd(R_CLK) & ~4U);
	gcc_sdcc1_set(1, 31, 1, 2);                        /* GPLL0 800M /16 * 1/2 = 25 MHz */
	set_clock_div(0);
	int st = card_state();
	con_puts(" REINIT STATE="); con_dec(st < 0 ? 100 - st : st); con_putc('\n');
	return st == 4 ? 0 : -14;
}

int sdhc_read(uint64_t lba, void *buf)
{
	return cmd(17, (uint32_t)lba, RSP_R1, -1, buf);
}

void sdhc_allow_writes(uint64_t lo, uint64_t hi)
{
	wr_lo = lo;
	wr_hi = hi;
}

int sdhc_write(uint64_t lba, const void *buf)
{
	if (lba < wr_lo || lba >= wr_hi)
		return 99;                                         /* outside allowed window */
	int e = cmd(24, (uint32_t)lba, RSP_R1, 1, (uint32_t *)buf);
	if (e)
		return e;
	uint64_t end = ticks() + 19200ULL * 1000;              /* wait for programming */
	while (ticks() < end) {
		int st = card_state();
		if (st == 4 && (last_resp & (1U << 8)))
			return 0;
	}
	return 6;
}
