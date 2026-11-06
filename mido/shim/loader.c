/*
 * Act as iBoot for xnu (adapted from ios-sweet shim/loader.c, MIT).
 *
 * tools/mkpayload.py resolves the kernel's chained fixups and appends the ADT,
 * so this only copies segments to phys_base + (vmaddr - virt_base), places
 * the ADT and boot_args behind the kernel and jumps to the entry point with
 * the MMU and caches off, x0 = boot_args (physical).
 *
 * mido specifics: payload arrives in LK's ramdisk (~0x43600000, found via
 * /chosen), RAM window is 0xA0000000 + 1.5 GB, framebuffer is the 32 bpp
 * RGB0 pipe set up by fb_to_32bpp().
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH 608
#define PAGE_16K         0x4000UL     /* 16K alignment also satisfies the 4K kernel */

#define MIDO_PHYS_BASE   0xa0000000UL
#define MIDO_RAM_END     0x100000000UL
#define FB_BASE          0x90001000UL
#define FB_W             1080
#define FB_H             1920
#define FB_STRIDE        (FB_W * 4)

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const volatile char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* Returns a reason string if it does not boot; never returns on success. */
const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "NO INITRD IN /CHOSEN";
	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "NO SWEETXNU PAYLOAD";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "BAD PAYLOAD HEADER";
	if (p->nsegs == 0 || p->adt_size == 0)
		return "PAYLOAD HAS NO KERNEL (HEADER-ONLY TEST)";
	if (p->phys_base != MIDO_PHYS_BASE || p->phys_base + p->mem_size > MIDO_RAM_END)
		return "PAYLOAD RAM WINDOW IS NOT MIDO'S";
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "RAMDISK OVERLAPS KERNEL MEMORY";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;   /* pa = va + this */
	con_kv("XNU VIRT BASE", p->virt_base);
	con_kv("XNU ENTRY", p->entry_va);
	con_kv("XNU NSEGS", p->nsegs);

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		if (dst < p->phys_base || dst + s->vmsize > p->phys_base + p->mem_size)
			return "SEGMENT OUTSIDE RAM WINDOW";
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;   /* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_kv("BOOT ARGS PA", ba_pa);
	con_kv("TOP OF KERNEL", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_kv("JUMP", entry_pa);
	con_puts("JUMPING TO XNU\n");

	/* Everything was written with the MMU and caches off; drop stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "XNU RETURNED";
}
