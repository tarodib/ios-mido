#pragma once
#include <stdint.h>

/* Ask the RPM (SMD rpm_requests) to enable PM8953 LDO `ldo` at uv microvolts, ma mA. */
int rpm_ldo_enable(unsigned ldo, uint32_t uv, uint32_t ma);

/* Vote the VDD_CX voltage level (PM8953 S2) through the RPM. */
int rpm_cx_level(uint32_t level);
