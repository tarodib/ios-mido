#pragma once
#include <stdint.h>

/* Adreno 506 stage 1: power domains, clocks at 19.2 MHz, TZ restore, register read. */
int gpu_probe(void);
/* Stage 2: CP microcode from the test ramdisk (MIDOGPU1 blob), ring, CP_MEM_WRITE test. */
int gpu_stage2(uint64_t dtb);
/* Stage 3: 2D blit of a CPU-drawn gradient into the framebuffer by the GPU. */
int gpu_stage3(void);
