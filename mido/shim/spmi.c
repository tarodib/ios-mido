/*
 * SPMI PMIC arbiter v2 (msm8953): core 0x0200F000, chnls 0x02400000, obsrvr 0x02C00000, EE 0.
 * Channel = APID found by scanning core+0x800+4n for the PPID (sid<<8 | peripheral).
 * Writes: EXT_WRITEL (opc 0); reads: EXT_READL (opc 1); one byte at a time.
 */
#include <stdint.h>
#include "spmi.h"

#define SPMI_CORE   0x0200f000UL
#define SPMI_CHNLS  0x02400000UL
#define SPMI_OBSRVR 0x02c00000UL
#define SPMI_EE     0

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

int spmi_find_chan(uint32_t ppid)
{
	for (int i = 0; i < 512; i++)
		if (((rd32(SPMI_CORE + 0x800 + 4 * i) >> 8) & 0xfff) == ppid)
			return i;
	return -1;
}

static uint32_t spmi_wait(uintptr_t base)
{
	uint32_t st = 0;
	for (int i = 0; i < 100000 && !(st = rd32(base + 0x08)); i++)
		;
	return st;   /* bit0 done, 1 failure, 2 denied, 3 dropped */
}

int spmi_read(int chan, uint8_t reg, uint8_t *val)
{
	uintptr_t b = SPMI_OBSRVR + 0x1000 * SPMI_EE + 0x8000UL * chan;
	wr32(b + 0x00, (1U << 27) | ((uint32_t)reg << 4));
	uint32_t st = spmi_wait(b);
	*val = rd32(b + 0x18) & 0xff;
	return st == 1 ? 0 : (int)(st ? st : 0xff);
}

int spmi_write(int chan, uint8_t reg, uint8_t val)
{
	uintptr_t b = SPMI_CHNLS + 0x1000 * SPMI_EE + 0x8000UL * chan;
	wr32(b + 0x10, val);
	wr32(b + 0x00, (0U << 27) | ((uint32_t)reg << 4));
	uint32_t st = spmi_wait(b);
	return st == 1 ? 0 : (int)(st ? st : 0xff);
}
