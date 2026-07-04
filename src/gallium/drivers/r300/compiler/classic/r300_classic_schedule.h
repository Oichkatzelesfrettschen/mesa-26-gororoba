/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_SCHEDULE_H
#define R300_CLASSIC_SCHEDULE_H

#include <stdbool.h>

struct radeon_compiler;

/* R300_CLASSIC_NEW_SCHED=1 routes a fragment program through
 * r300_classic_schedule in place of rc_pair_schedule.  Default off keeps the
 * legacy priority-score list scheduler, so gate-off output stays byte
 * identical (the byte-exact self-diff oracle). */
bool
r300_classic_new_sched_enabled(void);

/* Purpose-built R300 PFS pair scheduler over the post-rc_pair_translate
 * RC_INSTRUCTION_PAIR stream.  It forms a schedule directly from the pair
 * form's read/write data dependencies, honoring the presubtract-source
 * NOP-bubble hazard (US_ALU_RGB_INST bit 31), and leaves the stream
 * rc_pair_instruction-shaped so rc_pair_regalloc and the emitters are
 * unchanged.  A drop-in for rc_pair_schedule in r3xx_compile_fragment_program's
 * pass list, selected by r300_classic_new_sched_enabled().
 *
 * The pass handles the straight-line, single-assignment ALU-pair shape the
 * classic front end produces for the MOV/MAD/ADD/MUL subset.  A stream that
 * carries a TEX block, control flow, a still-normal instruction, or a re-used
 * destination index falls back to rc_pair_schedule, whose block/TEX/liveness
 * machinery the subset does not need. */
void
r300_classic_schedule(struct radeon_compiler *cc, void *user);

#endif /* R300_CLASSIC_SCHEDULE_H */
