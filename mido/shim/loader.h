#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload in LK's ramdisk. Returns a reason if it cannot. */
const char *loader_boot(uintptr_t dtb);
