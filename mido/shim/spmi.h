#pragma once
#include <stdint.h>

int spmi_find_chan(uint32_t ppid);
int spmi_read(int chan, uint8_t reg, uint8_t *val);
int spmi_write(int chan, uint8_t reg, uint8_t val);
