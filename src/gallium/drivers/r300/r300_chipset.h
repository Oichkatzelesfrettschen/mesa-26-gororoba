/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CHIPSET_H
#define R300_CHIPSET_H

#include "amd/r300/common/r300_capabilities.h"
#include "util/compiler.h"

/* these are sizes in dwords */
#define R300_HIZ_LIMIT 10240
#define RV530_HIZ_LIMIT 15360

/* rv3xx have only one pipe */
#define PIPE_ZMASK_SIZE 4096
#define RV3xx_ZMASK_SIZE 5120

void r300_parse_chipset(uint32_t pci_id, struct r300_capabilities* caps);

#endif /* R300_CHIPSET_H */
