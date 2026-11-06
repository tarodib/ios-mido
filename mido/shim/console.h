#pragma once
#include <stdint.h>

#define C_NAVY   0x002050
#define C_RED    0x800000
#define C_GREEN  0x005000

void con_clear(uint32_t rgb);
void con_set_32bpp(void);
void con_log_init(void);
uintptr_t con_log_phys(void);
void con_putc(char c);
void con_puts(const char *s);
void con_hex(uint64_t v, int digits);
void con_dec(uint64_t v);
void con_kv(const char *k, uint64_t v);
