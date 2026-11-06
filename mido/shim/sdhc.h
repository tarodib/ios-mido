#pragma once
#include <stdint.h>

int sdhc_init(void);
int sdhc_read(uint64_t lba, void *buf);
void sdhc_allow_writes(uint64_t lo, uint64_t hi);
int sdhc_write(uint64_t lba, const void *buf);
