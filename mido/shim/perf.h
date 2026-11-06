#pragma once

/* Boot CPU clock in MHz (PMU cycles vs CNTPCT) + CPU PLL / mux register dump. */
unsigned perf_probe(void);
#include <stdint.h>

/* Start the CPU PLL at 19.2 MHz * l and move both cluster muxes onto it (0 ok). */
int perf_cpu_pll(unsigned l);
/* Print the PM8953 S5 (APC rail) registers (read only). */
void perf_apc_diag(int (*spmi_rd)(int, uint8_t, uint8_t *), int (*find)(uint32_t));

/* Step the CPU clock up to 2016 MHz, checking a workload at each step. */
unsigned perf_sweep(void);

/* Highest stable CPU clock + CCI at 800 MHz, before handing off to XNU. */
void perf_boost(void);
