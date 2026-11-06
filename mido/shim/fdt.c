/*
 * Minimal flattened-device-tree reader: look up a property of /chosen.
 * LK (aboot) writes linux,initrd-start / linux,initrd-end there as 32- or
 * 64-bit big-endian cells, depending on the build; both are accepted.
 */
#include <stdint.h>
#include "fdt.h"

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

static uint32_t be32(uintptr_t p)
{
	const volatile uint8_t *b = (const volatile uint8_t *)p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static int streq(uintptr_t a, const char *b)
{
	const volatile char *s = (const volatile char *)a;
	while (*b)
		if (*s++ != *b++)
			return 0;
	return *s == 0;
}

static uintptr_t strend(uintptr_t p)
{
	while (*(const volatile char *)p)
		p++;
	return p;
}

int fdt_chosen_u64(uintptr_t dtb, const char *name, uint64_t *out)
{
	if (!dtb || be32(dtb) != FDT_MAGIC)
		return 0;
	uintptr_t st = dtb + be32(dtb + 8), strs = dtb + be32(dtb + 12);
	int depth = 0, in_chosen = 0;
	for (uintptr_t p = st;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			depth++;
			in_chosen = depth == 2 && streq(p, "chosen");
			p = (strend(p) + 4) & ~3UL;
		} else if (tok == FDT_END_NODE) {
			if (in_chosen && depth == 2)
				in_chosen = 0;
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq(strs + nameoff, name)) {
				if (len == 4) { *out = be32(val); return 1; }
				if (len == 8) { *out = (uint64_t)be32(val) << 32 | be32(val + 4); return 1; }
				return 0;
			}
			p = (val + len + 3) & ~3UL;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			return 0;   /* FDT_END or corrupt */
		}
	}
}
