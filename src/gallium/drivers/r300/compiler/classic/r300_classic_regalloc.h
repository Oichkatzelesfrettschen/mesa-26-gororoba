/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_REGALLOC_H
#define R300_CLASSIC_REGALLOC_H

#include "r300_classic_ir.h"

/* Register allocation on the SSA program.  The R300 temp file is a flat
 * array of vec4 slots addressed by a scalar index -- no register classes, no
 * contiguity, no pairing -- so allocation is a linear scan over the
 * straight-line instruction list: each value-producing instruction takes the
 * lowest free slot, and a slot returns to the pool at its value's last use.
 * One SSA value per slot; no coalescing and no channel packing. */

struct r300_classic_regalloc_result {
   /* Temp slot per ssa_id; R300_CLASSIC_NO_TEMP for sinks. */
   unsigned *temp_of_ssa;
   /* High-water slot count actually used. */
   unsigned num_temps;
   /* NULL when allocation fits the target's temp file; the named rejection
    * otherwise (ralloc'd on mem_ctx). */
   const char *reject_reason;
};

#define R300_CLASSIC_NO_TEMP (~0u)

/* Returns false only on out-of-memory; an over-budget program returns true
 * with temp_of_ssa NULL and reject_reason set. */
bool
r300_classic_regalloc(void *mem_ctx, const struct r300_classic_program *p,
                      struct r300_classic_regalloc_result *result);

#endif /* R300_CLASSIC_REGALLOC_H */
