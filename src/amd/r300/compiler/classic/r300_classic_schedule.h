/*
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
 * NOP-bubble hazard (US_ALU_RGB_INST bit 31) and RC_FILE_OUTPUT/depth
 * write-after-write ordering, and leaves the stream rc_pair_instruction-shaped
 * so rc_pair_regalloc and the emitters are unchanged.  A drop-in for
 * rc_pair_schedule in r3xx_compile_fragment_program's pass list, selected by
 * r300_classic_new_sched_enabled().
 *
 * Beyond reordering, the pass folds an independent RGB-only pair and an
 * independent Alpha-only pair into one full pair (rc_pair_try_merge, the same
 * source-slot/presubtract-reallocation machinery radeon_pair_schedule.c's
 * legacy merge uses) whenever both are simultaneously ready, and breaks ties
 * among several ready pairs by critical-path height rather than program
 * order, so the schedule is a genuine reordering-and-compaction decision, not
 * an identity permutation of rc_pair_translate's output.
 *
 * Temporary-register single assignment is tracked per (index, channel), so a
 * VEC-collect's disjoint per-channel writes into one temporary index are
 * legal (a real double write to the same channel still is not); output/depth
 * writes are tracked per (target, channel) as a write-after-write order
 * chain rather than an SSA fact, so two writers of disjoint channels of the
 * same render target are free to merge while two writers of the same channel
 * stay ordered.
 *
 * The pass handles the straight-line ALU-pair shape the classic front end
 * produces for the MOV/MAD/ADD/MUL/MIN/MAX/DP3/DP4 subset, and defers to
 * rc_pair_schedule -- on the untouched instruction list, never with a
 * hard-fail rc_error -- whenever it meets a shape it cannot safely schedule
 * itself: a TEX block, control flow, a still-normal instruction, a genuine
 * same-channel double write, a merged stream still longer than the target's
 * max_alu_insts envelope (only rc_pair_schedule's RGB<->Alpha *conversion*
 * search can still compact that further), a register index outside the pair
 * encoding's range, or a dependency graph with no ready node left to place. */
void
r300_classic_schedule(struct radeon_compiler *cc, void *user);

#endif /* R300_CLASSIC_SCHEDULE_H */
