/*
 * MIDO (Redmi Note 4, MSM8953) input: FT5435 touch, power / volume keys.
 *
 * The mido shim prepares the hardware before XNU starts (shim/input.c input_setup):
 * RPM ldoa10 2.85 V (touch vdd), BLSP1 AHB + QUP3 I2C clocks, gpio10/11 = blsp_i2c3
 * (mux 2), gpio65 (touch irq) and gpio85 (vol+) inputs with pull-up, FT5435 reset
 * released. This file only polls; it never writes the PMIC.
 *
 *   touch: BLSP1 QUP3 I2C @0x078b7000, FT5435 @0x38, v1 FIFO tags as Linux i2c-qup.
 *          Report regs 0x02 (count) + 6 bytes per point; panel 1080x1920; the
 *          capacitive keys report at y = 2040 (x 200 menu, 500 home, 800 back).
 *   keys:  power / vol-down = PM8953 PON INT_RT_STS (0x810) bits 0 / 1 through the
 *          SPMI arbiter v2 observer (EXT_READL); vol-up = TLMM gpio85, active low.
 * All verified on the device with the shim input test (2026-11-06).
 * Events go to SweetHID: sweet_hid_touch(finger, down, x, y), sweet_hid_key(key, down)
 * with key 0 power, 1 vol+, 2 vol-, 3 home (Csmr_Menu). Boot-arg midoinput=0 disables.
 */
#if defined(MIDO_BOARD)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_key(int key, int down);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void mido_input_init(void);

#define QUP_PA          0x078b7000u
#define TLMM_PA(n)      (0x01000000u + 0x1000u * (n))
#define SPMI_CORE_PA    0x0200f000u
#define SPMI_OBS_PA     0x02c00000u

#define Q_CONFIG        0x000
#define Q_STATE         0x004
#define Q_IO_MODE       0x008
#define Q_SW_RESET      0x00c
#define Q_OPER          0x018
#define Q_MX_OUT_CNT    0x100
#define Q_OUT_FIFO      0x110
#define Q_MX_WR_CNT     0x150
#define Q_MX_IN_CNT     0x200
#define Q_MX_RD_CNT     0x208
#define Q_IN_FIFO       0x218
#define Q_I2C_CLK       0x400
#define Q_I2C_STATUS    0x404
#define ST_VALID        (1u << 2)
#define OP_OUT_NOT_EMPTY (1u << 4)
#define OP_IN_NOT_EMPTY (1u << 5)
#define I2C_ERR_MASK    0x38000fcu
#define T_START         (1u << 8)
#define T_STOP          (3u << 8)
#define T_REC           (4u << 8)
#define FT_ADDR         0x38
#define FT_POINTS       2               /* 13-byte reads fit the QUP FIFO */
#define PANEL_W         1080
#define PANEL_H         1920

static vm_offset_t qup, gpio65, gpio85, spmi_core, spmi_obs;
static int pon_chan = -1;
static bool input_started;
static int input_chan;

static inline uint32_t rd(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(vm_offset_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

static uint64_t
deadline_us(uint64_t us)
{
	uint64_t d;
	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &d);
	return mach_absolute_time() + d;
}

/* ---------------- QUP I2C (polled FIFO mode) ---------------- */

static int
qup_state(uint32_t s)
{
	uint64_t end = deadline_us(10000);
	while (mach_absolute_time() < end) {
		uint32_t v = rd(qup, Q_STATE);
		if ((v & ST_VALID) && (v & 3) == s) {
			return 0;
		}
		if (v & ST_VALID) {
			wr(qup, Q_STATE, s);
		}
	}
	return -1;
}

static int
qup_xfer(const uint16_t *tags, int ntags, uint8_t *in, int nread)
{
	wr(qup, Q_SW_RESET, 1);
	uint64_t end = deadline_us(10000);
	while (!(rd(qup, Q_STATE) & ST_VALID) && mach_absolute_time() < end) {
	}
	if (qup_state(0)) {
		return -10;
	}
	wr(qup, Q_I2C_CLK, (3u << 8) | 21u);                       /* 400 kHz from 19.2 MHz */
	wr(qup, Q_MX_OUT_CNT, 0);
	wr(qup, Q_MX_WR_CNT, (uint32_t)ntags);
	wr(qup, Q_MX_IN_CNT, 0);
	wr(qup, Q_MX_RD_CNT, (uint32_t)nread);
	wr(qup, Q_CONFIG, (2u << 8) | 15u | (nread ? 0 : (1u << 7)));
	wr(qup, Q_IO_MODE, (1u << 14) | (1u << 15));
	wr(qup, Q_OPER, 0xff0);
	wr(qup, Q_I2C_STATUS, 0xfffffc);
	if (qup_state(1)) {
		return -11;
	}
	for (int i = 0; i < ntags; i += 2) {
		wr(qup, Q_OUT_FIFO, tags[i] | (i + 1 < ntags ? (uint32_t)tags[i + 1] << 16 : 0));
	}
	int got = 0;
	end = deadline_us(20000);
	while (mach_absolute_time() < end) {
		uint32_t st = rd(qup, Q_I2C_STATUS), op = rd(qup, Q_OPER);
		if (st & I2C_ERR_MASK) {
			qup_state(0);
			return -3;
		}
		while (got < nread && (rd(qup, Q_OPER) & OP_IN_NOT_EMPTY)) {
			uint32_t v = rd(qup, Q_IN_FIFO);
			in[got++] = (uint8_t)v;
			if (got < nread) {
				in[got++] = (uint8_t)(v >> 16);
			}
		}
		if (got >= nread && !(op & OP_OUT_NOT_EMPTY) && !(st & (1u << 8))) {
			break;
		}
	}
	qup_state(0);
	return got < nread ? -4 : 0;
}

static int
ft_read(uint8_t reg, uint8_t *buf, int n)
{
	uint16_t w[2] = { (uint16_t)(T_START | (FT_ADDR << 1)), (uint16_t)(T_STOP | reg) };
	int e = qup_xfer(w, 2, NULL, 0);
	if (e) {
		return e;
	}
	uint16_t r[2] = { (uint16_t)(T_START | (FT_ADDR << 1) | 1), (uint16_t)(T_REC | (unsigned)n) };
	return qup_xfer(r, 2, buf, n);
}

/* ---------------- SPMI observer read (PON) ---------------- */

static bool
pon_rt(uint8_t *val)
{
	if (pon_chan < 0 || !spmi_obs) {
		return false;
	}
	wr(spmi_obs, 0x00, (1u << 27) | (0x10u << 4));            /* EXT_READL, 1 byte, reg 0x10 */
	uint64_t end = deadline_us(1000);
	uint32_t st = 0;
	while (!(st = rd(spmi_obs, 0x08)) && mach_absolute_time() < end) {
	}
	if (st != 1) {
		return false;
	}
	*val = (uint8_t)rd(spmi_obs, 0x18);
	return true;
}

/* ---------------- poller ---------------- */

static void
input_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&input_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
input_thread(void *arg __unused, wait_result_t wr_ __unused)
{
	int key_state[3] = {0, 0, 0};
	bool fdown[FT_POINTS] = {false}, home_down = false;
	int fx[FT_POINTS] = {0}, fy[FT_POINTS] = {0};
	unsigned logged = 0, errors = 0;

	for (;;) {
		/* keys */
		uint8_t pon = 0;
		bool pok = pon_rt(&pon);
		int now[3] = {
			pok ? !!(pon & 1u) : key_state[0],
			!(rd(gpio85, 4) & 1u),
			pok ? !!(pon & 2u) : key_state[2],
		};
		for (int k = 0; k < 3; k++) {
			if (now[k] != key_state[k]) {
				key_state[k] = now[k];
				printf("MIDO INPUT: key %d %s\n", k, now[k] ? "down" : "up");
				sweet_hid_key(k, now[k]);
			}
		}

		/* touch: read while the IRQ line is low or a finger/key is still down */
		bool irq = !(rd(gpio65, 4) & 1u);
		bool any = home_down;
		for (int i = 0; i < FT_POINTS; i++) {
			any |= fdown[i];
		}
		if (irq || any) {
			uint8_t b[1 + 6 * FT_POINTS];
			int e = ft_read(0x02, b, (int)sizeof(b));
			if (e) {
				if ((errors++ % 500) == 0) {
					printf("MIDO INPUT: touch read error %d\n", e);
				}
			} else {
				unsigned n = b[0] & 0x0f;
				bool seen[FT_POINTS] = {false}, home = false;
				if (n <= 10) {                                     /* 0xff while fw boots */
					for (unsigned t = 0; t < n && t < FT_POINTS; t++) {
						const uint8_t *p = b + 1 + 6 * t;
						unsigned ev = p[0] >> 6;
						int x = (p[0] & 0x0f) << 8 | p[1];
						unsigned id = p[2] >> 4;
						int y = (p[2] & 0x0f) << 8 | p[3];
						if (ev == 1 || id >= FT_POINTS) {          /* lift event / extra finger */
							continue;
						}
						if (y >= PANEL_H) {                        /* capacitive key row */
							home |= (x > 350 && x < 650);
							continue;
						}
						seen[id] = true;
						if (!fdown[id] || x != fx[id] || y != fy[id]) {
							sweet_hid_touch((int)id, 1, x, y);
							if (logged < 6) {
								logged++;
								printf("MIDO INPUT: touch %u at %d,%d\n", id, x, y);
							}
						}
						fdown[id] = true;
						fx[id] = x;
						fy[id] = y;
					}
					for (int i = 0; i < FT_POINTS; i++) {
						if (fdown[i] && !seen[i]) {
							sweet_hid_touch(i, 0, fx[i], fy[i]);
							fdown[i] = false;
						}
					}
					if (home != home_down) {
						home_down = home;
						printf("MIDO INPUT: home key %s\n", home ? "down" : "up");
						sweet_hid_key(3, home);
					}
				}
			}
		}
		input_wait(any ? 10 : 15);
	}
}

void
mido_input_init(void)
{
	int enabled = 1;
	thread_t thread;

	if (input_started) {
		return;
	}
	input_started = true;
	PE_parse_boot_argn("midoinput", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("MIDO INPUT: disabled (midoinput=0)\n");
		return;
	}
	qup = ml_io_map(QUP_PA, 0x1000);
	gpio65 = ml_io_map(TLMM_PA(65), 0x1000);
	gpio85 = ml_io_map(TLMM_PA(85), 0x1000);
	spmi_core = ml_io_map(SPMI_CORE_PA, 0x1000);
	if (!qup || !gpio65 || !gpio85 || !spmi_core) {
		printf("MIDO INPUT: MMIO mapping failed\n");
		return;
	}
	for (int i = 0; i < 512; i++) {
		if (((rd(spmi_core, 0x800 + 4u * (uint32_t)i) >> 8) & 0xfff) == 0x008) {
			pon_chan = i;
			break;
		}
	}
	if (pon_chan >= 0) {
		spmi_obs = ml_io_map(SPMI_OBS_PA + 0x8000u * (uint32_t)pon_chan, 0x1000);
	}
	uint8_t id[1] = {0};
	int e = ft_read(0xa3, id, 1);
	printf("MIDO INPUT: FT5435 id 0x%02x (err %d), PON chan %d, vol+ gpio85=%u\n",
	    id[0], e, pon_chan, rd(gpio85, 4) & 1u);
	if (kernel_thread_start(input_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* MIDO_BOARD */
