#pragma once
#include <stdint.h>

int sdhc_init(void);
int sdhc_read(uint64_t lba, void *buf);
void sdhc_allow_writes(uint64_t lo, uint64_t hi);
int sdhc_write(uint64_t lba, const void *buf);

/* 8-bit bus + HS 50 MHz with read-back checks; bench_lba != 0 also times 1 MiB. */
int sdhc_fast(uint64_t bench_lba, uint64_t wtest_lba);
