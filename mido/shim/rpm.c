/*
 * Minimal RPM client for the mido shim: turn on an RPM-owned regulator (e.g. ldoa 10,
 * the FT5435 touch supply) through the SMD "rpm_requests" channel, as LK's
 * msm_shared/smd.c + rpm-smd.c and Linux qcom_smd/qcom_smd-rpm do. A direct SPMI write
 * to an RPM-owned LDO is an arbiter violation and resets the SoC (input test run 2).
 *
 * msm8953 (mido.dtb): SMEM 0x86300000, IPC doorbell 0x0b011008 bit 0, edge 0xf.
 * SMEM legacy TOC: proc_comm (64 B) + version (128 B) + heap_info (16 B), then
 * entries {allocated, offset, size, base_ext} (16 B). Items: 13 channel alloc table
 * {name[20], cid, ctype, ref}, 14+cid port info {tx half, rx half}, 338+cid FIFOs
 * (tx first, rx second, size/2 each). LK closes the channel in target_uninit; we
 * reopen it. MMU is off: all accesses are Device memory (coherent with the RPM).
 */
#include <stdint.h>
#include "console.h"
#include "rpm.h"

#define SMEM_BASE     0x86300000UL
#define SMEM_TOC      (SMEM_BASE + 64 + 128 + 16)
#define IPC_REG       0x0b011008UL
#define ID_ALLOC_TBL  13
#define ID_SMD_BASE   14
#define ID_SMD_FIFO   338

enum { SS_CLOSED = 0, SS_OPENING = 1, SS_OPENED = 2 };

struct half {                     /* Linux smd_channel_info / LK smd_half_channel */
	volatile uint32_t state, fDSR, fCTS, fCD, fRI, fHEAD, fTAIL, fSTATE, fBLOCKREADINTR;
	volatile uint32_t tail, head;
};

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uintptr_t smem_item(unsigned id, uint32_t *size)
{
	uintptr_t e = SMEM_TOC + 16UL * id;
	if (!rd(e))
		return 0;
	uint32_t off = rd(e + 4), sz = rd(e + 8), ext = rd(e + 12);
	if (size)
		*size = sz;
	return (ext ? (uintptr_t)ext : SMEM_BASE) + off;
}

static void kick(void) { dsb(); wr(IPC_REG, 1u); }

static int wait_remote(struct half *rx, uint32_t want, unsigned ms)
{
	uint64_t end = now() + 19200ULL * ms;
	while (now() < end) {
		if (rx->state == want)
			return 0;
	}
	return -1;
}

static struct half *tx, *rx;
static uintptr_t txf, rxf;
static uint32_t fifo_sz, msg_id = 0x4d494430;   /* "MID0" */

static int rpm_open(void)
{
	if (tx)
		return 0;
	uint32_t sz = 0;
	uintptr_t tbl = smem_item(ID_ALLOC_TBL, &sz);
	con_puts(" RPM: SMEM TBL="); con_hex(tbl, 8); con_puts(" SZ="); con_hex(sz, 4);
	if (!tbl) { con_puts(" NONE\n"); return -1; }
	int cid = -1;
	for (unsigned i = 0; i < sz / 32 && i < 64; i++) {
		const volatile char *n = (const volatile char *)(tbl + 32UL * i);
		static const char want[] = "rpm_requests";
		unsigned k = 0;
		while (k < sizeof(want) && n[k] == want[k])
			k++;
		if (k == sizeof(want) && (rd(tbl + 32UL * i + 24) & 0xff) == 0xf) {
			cid = (int)rd(tbl + 32UL * i + 20);
			break;
		}
	}
	con_puts(" CID="); con_dec((uint64_t)(int64_t)cid); con_putc('\n');
	if (cid < 0)
		return -2;
	uintptr_t pi = smem_item(ID_SMD_BASE + (unsigned)cid, 0);
	uintptr_t ff = smem_item(ID_SMD_FIFO + (unsigned)cid, &sz);
	if (!pi || !ff)
		return -3;
	tx = (struct half *)pi;
	rx = (struct half *)(pi + sizeof(struct half));
	fifo_sz = sz / 2;
	txf = ff;
	rxf = ff + fifo_sz;
	con_puts(" PORT="); con_hex(pi, 8); con_puts(" FIFO="); con_hex(ff, 8); con_puts("/"); con_hex(fifo_sz, 4);
	con_puts(" TX ST="); con_dec(tx->state); con_puts(" RX ST="); con_dec(rx->state); con_putc('\n');

	if (tx->state != SS_OPENED || rx->state != SS_OPENED) {
		/* reset our half, then OPENING -> (remote OPENED) -> OPENED */
		tx->state = SS_CLOSED; tx->fDSR = tx->fCTS = tx->fCD = 0;
		tx->head = tx->tail = 0;
		rx->tail = 0;
		tx->fSTATE = 1;
		kick();
		tx->state = SS_OPENING;
		tx->fSTATE = 1;
		kick();
		int e = wait_remote(rx, SS_OPENED, 200);
		if (e) {                                   /* some RPM fw answer OPENING first */
			e = wait_remote(rx, SS_OPENING, 1) ? -4 : 0;
		}
		tx->fDSR = tx->fCTS = tx->fCD = 1;
		tx->state = SS_OPENED;
		tx->fSTATE = 1;
		kick();
		wait_remote(rx, SS_OPENED, 200);
		con_puts(" REOPEN: TX ST="); con_dec(tx->state); con_puts(" RX ST="); con_dec(rx->state); con_putc('\n');
		if (rx->state != SS_OPENED) {
			tx = 0;
			return -5;
		}
	}
	return 0;
}

static void fifo_put(const uint32_t *w, unsigned nwords)
{
	uint32_t h = tx->head;
	for (unsigned i = 0; i < nwords; i++) {
		wr(txf + h, w[i]);
		h += 4;
		if (h >= fifo_sz)
			h = 0;
	}
	dsb();
	tx->head = h;
}

/* Send one RPM "req" (active set) with nkvp key/value words pairs; returns 0 on ack. */
static int rpm_req(uint32_t type, uint32_t id, const uint32_t *kvp, unsigned nwords)
{
	uint32_t pkt[5 + 7 + 16];
	unsigned dlen = nwords * 4, n = 0;
	uint32_t body = 7 * 4 + dlen;                     /* rpm header + request + data */
	pkt[n++] = body; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0;   /* SMD pkt hdr */
	pkt[n++] = 0x00716572;                            /* "req" */
	pkt[n++] = 5 * 4 + dlen;                          /* length after service_type/length */
	pkt[n++] = ++msg_id;
	pkt[n++] = 0;                                     /* active set */
	pkt[n++] = type;
	pkt[n++] = id;
	pkt[n++] = dlen;
	for (unsigned i = 0; i < nwords && n < sizeof(pkt) / 4; i++)
		pkt[n++] = kvp[i];
	uint32_t used = tx->head >= tx->tail ? tx->head - tx->tail : tx->head + fifo_sz - tx->tail;
	uint32_t free = fifo_sz - used - 4;
	if (free < n * 4)
		return -6;
	fifo_put(pkt, n);
	tx->fHEAD = 1;
	tx->fBLOCKREADINTR = 0;
	kick();
	/* wait for the ack in our rx fifo and consume everything there */
	uint64_t end = now() + 19200ULL * 200;
	while (now() < end && rx->head == rx->tail)
		;
	uint32_t got = rx->head >= rx->tail ? rx->head - rx->tail : rx->head + fifo_sz - rx->tail;
	uint32_t first[8] = {0};
	for (unsigned i = 0; i < 8 && i * 4 < got; i++)
		first[i] = rd(rxf + ((rx->tail + 4 * i) % fifo_sz));
	rx->tail = rx->head;
	tx->fTAIL = 1;
	kick();
	con_puts(" RPM ACK BYTES="); con_dec(got);
	con_puts(" W5="); con_hex(first[5], 8); con_puts(" W7="); con_hex(first[7], 8); con_putc('\n');
	return got ? 0 : -7;
}

int rpm_ldo_enable(unsigned ldo, uint32_t uv, uint32_t ma)
{
	int e = rpm_open();
	if (e) {
		con_puts(" RPM OPEN FAILED E="); con_dec((uint64_t)(int64_t)e); con_putc('\n');
		return e;
	}
	const uint32_t kvp[] = {
		0x6e657773, 4, 1,          /* "swen" = 1 */
		0x00007675, 4, uv,         /* "uv" */
		0x0000616d, 4, ma,         /* "ma" */
	};
	return rpm_req(0x616f646c, ldo, kvp, sizeof(kvp) / 4);   /* "ldoa" */
}
