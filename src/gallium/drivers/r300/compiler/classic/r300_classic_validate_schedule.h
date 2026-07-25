/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_VALIDATE_SCHEDULE_H
#define R300_CLASSIC_VALIDATE_SCHEDULE_H

#include <stdbool.h>

struct radeon_compiler;

/* Oracle (b), schedule legality: walks the RC_INSTRUCTION_PAIR stream of an
 * already-scheduled fragment program (rc_pair_translate, rc_pair_schedule,
 * and rc_pair_regalloc have all run) and checks the register-documented
 * constraints a scheduler must honor, independent of whether the resulting
 * colors happen to be correct -- a schedule that violates one of these can
 * still pass the CPU-evaluator parity check by accident (the evaluator
 * models RC opcodes, not the RGB/Alpha pipe coupling) while producing wrong
 * pixels on real silicon.  Checks:
 *
 *   - RGB.REPL_ALPHA (the R300_ALU_OUTC_REPL_ALPHA / R500_ALU_RGBA_OP_SOP
 *     hardware encoding) always has a live Alpha producer in the same pair,
 *     since REPL_ALPHA carries no RGB result of its own.
 *   - Alpha.DP3/DP4 (the R300_ALU_OUTA_DP4 / R500_ALPHA_OP_DP encoding)
 *     always pairs with an RGB lane computing the SAME dot-product opcode
 *     and reading the same operands, since Alpha's DP mode broadcasts RGB's
 *     result rather than computing its own.
 *   - WriteALUResult and an output write -- RGB.OutputWriteMask,
 *     Alpha.OutputWriteMask, or Alpha.DepthWriteMask -- are never both set
 *     on one pair (they share encoding bits on both targets,
 *     r500_fragprog_emit.c's emit_paired).
 *   - A presubtract source that reads the immediately preceding pair's
 *     destination carries a NOP bubble on that preceding pair
 *     (US_ALU_RGB_INST bit 31, radeon_pair_schedule.c's presub_nop()).
 *
 * On the first violation, returns false and, if reject_reason is non-NULL,
 * sets *reject_reason to a static string naming the constraint that failed.
 * reject_reason is optional and is cleared to NULL on entry when supplied,
 * so a caller never observes a reason left over from an earlier call. */
bool
r300_classic_validate_schedule(struct radeon_compiler *c, const char **reject_reason);

#endif /* R300_CLASSIC_VALIDATE_SCHEDULE_H */
