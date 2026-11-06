#pragma once
#include <stdint.h>

/* Read a 32- or 64-bit property of /chosen. Returns 1 on success. */
int fdt_chosen_u64(uintptr_t dtb, const char *name, uint64_t *out);
