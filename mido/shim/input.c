/*
 * mido input bring-up test (shim only, no XNU): FT5435 touch over BLSP1 QUP3 I2C,
 * power / volume-down from PM8953 PON, volume-up on TLMM GPIO 85.
 *
 * Hardware (stock mido.dtb, OrangeFox probe 2026-11-06):
 *   i2c@78b7000 (qcom,i2c-msm-v2, BLSP1 QUP3), 400 kHz, pins gpio10/11 func blsp_i2c3
 *   focaltech,5435 @0x38, reset gpio64, irq gpio65, 1080x1920, vkeys at y=2040
 *   vdd = pm8953_l10 2.85 V, vcc_i2c = pm8953_l6 1.8 V (both on in OrangeFox)
 *   gpio-keys vol_up = gpio85 active low; qpnp-power-on @0x800: kpdpwr, resin (vol-down)
 * QUP v1 FIFO mode as Linux i2c-qup.c (16-bit tags, START/DATA/STOP/REC).
 * Every wait has a timeout; nothing here writes storage.
 */
#include <stdint.h>
#include "console.h"
#include "spmi.h"
#include "input.h"
#include "rpm.h"

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static void delay_us(unsigned us) { uint64_t e = now() + 19200ULL * us / 1000; while (now() < e) ; }
static void delay_ms(unsigned ms) { delay_us(ms * 1000); }

/* GCC */
#define GCC_APCS_CLK_VOTE   0x01845004UL   /* bit 10: gcc_blsp1_ahb_clk */
#define GCC_BLSP1_AHB_CBCR  0x01801008UL
#define GCC_QUP3_I2C_RCGR   0x01804000UL
#define GCC_QUP3_I2C_CBCR   0x01804020UL
/* TLMM */
#define TLMM_CFG(n)   (0x01000000UL + 0x1000UL * (n))
#define TLMM_IO(n)    (0x01000004UL + 0x1000UL * (n))
/* QUP3 */
#define QUP           0x078b7000UL
#define Q_CONFIG      (QUP + 0x000)
#define Q_STATE       (QUP + 0x004)
#define Q_IO_MODE     (QUP + 0x008)
#define Q_SW_RESET    (QUP + 0x00c)
#define Q_OPER        (QUP + 0x018)
#define Q_ERR_FLAGS   (QUP + 0x01c)
#define Q_HW_VER      (QUP + 0x030)
#define Q_MX_OUT_CNT  (QUP + 0x100)
#define Q_OUT_FIFO    (QUP + 0x110)
#define Q_MX_WR_CNT   (QUP + 0x150)
#define Q_MX_IN_CNT   (QUP + 0x200)
#define Q_MX_RD_CNT   (QUP + 0x208)
#define Q_IN_FIFO     (QUP + 0x218)
#define Q_I2C_CLK     (QUP + 0x400)
#define Q_I2C_STATUS  (QUP + 0x404)

#define ST_VALID      (1u << 2)
#define OP_IN_NOT_EMPTY (1u << 5)
#define OP_OUT_NOT_EMPTY (1u << 4)
#define I2C_ERR_MASK  0x38000fcu
#define T_START       (1u << 8)
#define T_DATA        (2u << 8)
#define T_STOP        (3u << 8)
#define T_REC         (4u << 8)
#define FT_ADDR       0x38

static int qup_state(uint32_t s)
{
	for (int i = 0; i < 1000; i++) {
		uint32_t v = rd(Q_STATE);
		if ((v & ST_VALID) && (v & 3) == s)
			return 0;
		if (v & ST_VALID) {
			wr(Q_STATE, s);
		}
		delay_us(10);
	}
	return -1;
}

static int qup_reset(void)
{
	wr(Q_SW_RESET, 1);
	for (int i = 0; i < 1000; i++) {
		if (rd(Q_STATE) & ST_VALID)
			break;
		delay_us(10);
	}
	if (qup_state(0))
		return -1;
	/* 19.2 MHz XO source -> 400 kHz: fs_div = (19.2M/400k)/2 - 3 = 21, hs_div 3 */
	wr(Q_I2C_CLK, (3u << 8) | 21u);
	return 0;
}

static uint32_t last_status, last_oper;

/* One transaction: ntags 16-bit tags out, nread bytes in. 0 ok, <0 error. */
static int qup_xfer(const uint16_t *tags, int ntags, uint8_t *in, int nread)
{
	if (qup_reset())
		return -10;
	wr(Q_MX_OUT_CNT, 0);
	wr(Q_MX_WR_CNT, (uint32_t)ntags);
	wr(Q_MX_IN_CNT, 0);
	wr(Q_MX_RD_CNT, (uint32_t)nread);
	wr(Q_CONFIG, (2u << 8) | 15u | (nread ? 0 : (1u << 7)));   /* I2C mini core, N=16, NO_INPUT */
	wr(Q_IO_MODE, (1u << 14) | (1u << 15));                   /* FIFO, unpack + pack */
	wr(Q_OPER, 0xff0);
	wr(Q_I2C_STATUS, 0xfffffc);
	if (qup_state(1))
		return -11;
	for (int i = 0; i < ntags; i += 2) {
		uint32_t w = tags[i] | (i + 1 < ntags ? (uint32_t)tags[i + 1] << 16 : 0);
		wr(Q_OUT_FIFO, w);
	}
	int got = 0;
	uint64_t end = now() + 19200ULL * 20;                      /* 20 ms */
	while (now() < end) {
		uint32_t st = rd(Q_I2C_STATUS), op = rd(Q_OPER);
		last_status = st; last_oper = op;
		if (st & I2C_ERR_MASK) {
			qup_state(0);
			return (st & (1u << 3)) ? -2 : -3;                /* bit3 ~ NACK */
		}
		while (got < nread && (rd(Q_OPER) & OP_IN_NOT_EMPTY)) {
			uint32_t v = rd(Q_IN_FIFO);
			in[got++] = (uint8_t)v;
			if (got < nread)
				in[got++] = (uint8_t)(v >> 16);
		}
		if (got >= nread && !(op & OP_OUT_NOT_EMPTY) && !(st & (1u << 8)))
			break;                                             /* fifo drained, bus idle */
	}
	qup_state(0);
	if (got < nread)
		return -4;
	return 0;
}

static int ft_read(uint8_t reg, uint8_t *buf, int n)
{
	uint16_t w[2] = { (uint16_t)(T_START | (FT_ADDR << 1)), (uint16_t)(T_STOP | reg) };
	int e = qup_xfer(w, 2, 0, 0);
	if (e)
		return e;
	uint16_t r[2] = { (uint16_t)(T_START | (FT_ADDR << 1) | 1), (uint16_t)(T_REC | (unsigned)n) };
	return qup_xfer(r, 2, buf, n);
}

static void ldo_diag(const char *name, uint32_t ppid)
{
	int ch = spmi_find_chan(ppid);
	uint8_t st = 0, en = 0, v1 = 0, v2 = 0;
	if (ch >= 0) {
		spmi_read(ch, 0x08, &st);
		spmi_read(ch, 0x46, &en);
		spmi_read(ch, 0x40, &v1);
		spmi_read(ch, 0x41, &v2);
	}
	con_puts(name); con_puts(" CH="); con_dec((uint64_t)(int64_t)ch);
	con_puts(" ST="); con_hex(st, 2); con_puts(" EN="); con_hex(en, 2);
	con_puts(" V="); con_hex(v1, 2); con_hex(v2, 2); con_putc('\n');
}

/*
 * Before every XNU boot: everything XNU's read-only mido_input.c relies on.
 * Verified by input_test (2026-11-06): RPM ldoa10 -> VREG OK at 2.85 V, QUP HW
 * 0x20070000, blsp_i2c3 = mux 2 on gpio10/11, FT5435 id 0x54, touches 0..1080/0..1920.
 */
int input_setup(void)
{
	int e = rpm_ldo_enable(10, 2850000, 40);              /* touch vdd */
	wr(GCC_APCS_CLK_VOTE, rd(GCC_APCS_CLK_VOTE) | (1u << 10));
	wr(GCC_QUP3_I2C_CBCR, rd(GCC_QUP3_I2C_CBCR) | 1u);
	delay_ms(2);
	wr(TLMM_CFG(10), 2u << 2);                          /* blsp_i2c3, 2 mA, no pull */
	wr(TLMM_CFG(11), 2u << 2);
	wr(TLMM_CFG(65), 3u);                               /* touch irq: input, pull-up */
	wr(TLMM_CFG(85), 3u);                               /* vol+: input, pull-up */
	wr(TLMM_CFG(64), (1u << 9) | (1u << 6));            /* touch reset: output */
	wr(TLMM_IO(64), 0);
	delay_ms(10);
	wr(TLMM_IO(64), 2);                                 /* release: fw boots ~200 ms */
	uint32_t ahb = rd(GCC_BLSP1_AHB_CBCR), core = rd(GCC_QUP3_I2C_CBCR);
	con_puts("INPUT SETUP: RPM L10 E="); con_dec((uint64_t)(int64_t)e);
	con_puts(" CLK "); con_puts(((ahb | core) & (1u << 31)) ? "OFF" : "ON");
	con_putc('\n');
	return e;
}

void input_test(unsigned seconds)
{
	con_puts("\nINPUT TEST\n");
	ldo_diag(" L6 ", 0x145);
	ldo_diag(" L10", 0x149);
	/*
	 * Run 1: L10 (touch vdd, V=0x54 ~ 2.8 V already programmed) is off after LK;
	 * OrangeFox's kernel enables it itself. Enable it (EN_CTL 0x46 bit 7) and wait
	 * for VREG_OK (STATUS 0x08 bit 7). The arbiter may refuse if RPM owns it.
	 */
	/* Run 2: a direct SPMI write to L10 reset the SoC (RPM-owned): ask the RPM instead. */
	{
		int e = rpm_ldo_enable(10, 2850000, 40);
		int ch = spmi_find_chan(0x149);
		uint8_t st = 0;
		for (int i = 0; i < 100 && ch >= 0; i++) {
			spmi_read(ch, 0x08, &st);
			if (st & 0x80)
				break;
			delay_ms(1);
		}
		con_puts(" L10 VIA RPM: E="); con_dec((uint64_t)(int64_t)e);
		con_puts(" ST="); con_hex(st, 2); con_puts((st & 0x80) ? " VREG OK\n" : " NOT OK\n");
		ldo_diag(" L10", 0x149);
		if (!(st & 0x80)) {
			con_puts(" NO TOUCH POWER\n");
		}
		delay_ms(20);
	}

	/* clocks: BLSP1 AHB vote, QUP3 I2C core (RCG default = XO 19.2 MHz) */
	wr(GCC_APCS_CLK_VOTE, rd(GCC_APCS_CLK_VOTE) | (1u << 10));
	wr(GCC_QUP3_I2C_CBCR, rd(GCC_QUP3_I2C_CBCR) | 1u);
	delay_ms(2);
	uint32_t ahb = rd(GCC_BLSP1_AHB_CBCR), core = rd(GCC_QUP3_I2C_CBCR);
	con_puts(" AHB CBCR="); con_hex(ahb, 8); con_puts(" I2C CBCR="); con_hex(core, 8);
	con_puts(" RCGR="); con_hex(rd(GCC_QUP3_I2C_RCGR + 4), 8); con_putc('\n');
	if ((ahb | core) & (1u << 31)) {
		con_puts(" CLOCK STILL OFF - NOT TOUCHING QUP\n");
		return;
	}
	con_puts(" QUP HW VER="); con_hex(rd(Q_HW_VER), 8);
	con_puts(" IO_MODE="); con_hex(rd(Q_IO_MODE), 8); con_putc('\n');

	/* touch reset gpio64: output; irq gpio65 input pull-up; vol-up gpio85 input pull-up */
	wr(TLMM_CFG(64), (1u << 9) | (1u << 6));           /* func 0, OE, 4 mA */
	wr(TLMM_CFG(65), 3u);                               /* input, pull-up */
	wr(TLMM_CFG(85), 3u);
	wr(TLMM_IO(64), 0);
	delay_ms(10);
	wr(TLMM_IO(64), 2);                                 /* release reset */
	delay_ms(250);

	/* find the blsp_i2c3 mux number: read FT5435 ID registers */
	static const uint32_t funcs[4] = {3, 1, 2, 4};
	int fsel = -1;
	for (int i = 0; i < 4 && fsel < 0; i++) {
		uint32_t f = funcs[i];
		wr(TLMM_CFG(10), f << 2);                       /* func f, 2 mA, no pull */
		wr(TLMM_CFG(11), f << 2);
		delay_ms(2);
		uint8_t id[2] = {0, 0};
		int e = ft_read(0xa3, id, 1);
		int e2 = e ? e : ft_read(0xa8, id + 1, 1);
		con_puts(" FUNC "); con_dec(f); con_puts(": E="); con_dec((uint64_t)(int64_t)e2);
		con_puts(" ST="); con_hex(last_status, 8); con_puts(" OP="); con_hex(last_oper, 8);
		con_puts(" A3="); con_hex(id[0], 2); con_puts(" A8="); con_hex(id[1], 2); con_putc('\n');
		if (!e2)
			fsel = (int)f;
	}
	if (fsel < 0)
		con_puts(" NO I2C ANSWER FROM FT5435\n");

	/* PON for power / vol-down */
	int pon = spmi_find_chan(0x008);
	con_puts(" PON CH="); con_dec((uint64_t)(int64_t)pon); con_putc('\n');
	con_puts(" TOUCH THE SCREEN AND PRESS POWER / VOL+ / VOL- NOW\n");

	uint64_t end = now() + 19200000ULL * seconds;
	uint8_t last_n = 0xff, last_keys = 0xff;
	int lines = 0;
	while (now() < end && lines < 40) {
		uint8_t rt = 0, keys = 0;
		if (pon >= 0)
			spmi_read(pon, 0x10, &rt);                  /* INT_RT_STS: b0 KPDPWR, b1 RESIN */
		keys = (rt & 1) | ((rt & 2) ? 4 : 0) | ((rd(TLMM_IO(85)) & 1) ? 0 : 2);
		if (keys != last_keys) {
			con_puts(" KEYS: PWR="); con_dec(keys & 1); con_puts(" VOL+="); con_dec((keys >> 1) & 1);
			con_puts(" VOL-="); con_dec((keys >> 2) & 1); con_putc('\n');
			last_keys = keys;
			lines++;
		}
		if (fsel >= 0) {
			uint8_t b[16];
			if (!ft_read(0x02, b, 13)) {
				uint8_t n = b[0] & 0x0f;
				static unsigned held;
				held = n ? held + 1 : 0;
				if (n != last_n || (n && (held % 4) == 0)) {   /* held finger: ~every 400 ms */
					con_puts(" TOUCH N="); con_dec(n);
					for (int t = 0; t < n && t < 2; t++) {
						const uint8_t *p = b + 1 + 6 * t;
						unsigned ev = p[0] >> 6, x = (p[0] & 0xfu) << 8 | p[1];
						unsigned id = p[2] >> 4, y = (p[2] & 0xfu) << 8 | p[3];
						con_puts(" ["); con_dec(id); con_puts(" E"); con_dec(ev);
						con_puts(" "); con_dec(x); con_puts(","); con_dec(y); con_puts("]");
					}
					con_putc('\n');
					lines++;
					last_n = n;
				}
			}
		}
		delay_ms(100);
	}
	con_puts("INPUT TEST DONE\n");
}
