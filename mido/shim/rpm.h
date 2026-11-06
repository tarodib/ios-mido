#pragma once
#include <stdint.h>

/* Ask the RPM (SMD rpm_requests) to enable PM8953 LDO `ldo` at uv microvolts, ma mA. */
int rpm_ldo_enable(unsigned ldo, uint32_t uv, uint32_t ma);
