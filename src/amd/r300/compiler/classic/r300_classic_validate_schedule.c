/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_validate_schedule.h"

#include <stddef.h>

#include "../radeon_compiler.h"
#include "../radeon_opcodes.h"
#include "../radeon_program.h"
#include "../radeon_program_constants.h"
#include "../radeon_program_pair.h"

/* RC_OPCODE_REPL_ALPHA in the RGB slot is the R300_ALU_OUTC_REPL_ALPHA
 * (r300_fragprog_emit.c translate_rgb_opcode) / R500_ALU_RGBA_OP_SOP
 * (r500_fragprog_emit.c translate_rgb_op) hardware encoding: the RGB pipe
 * outputs whatever the Alpha pipe computes THE SAME CYCLE, carrying no
 * independent RGB result of its own.  A pair whose RGB lane is REPL_ALPHA
 * therefore requires an active Alpha producer in the same slot. */
static bool
check_rgb_reads_alpha(const struct rc_pair_instruction *p, const char **why)
{
   if (p->RGB.Opcode == RC_OPCODE_REPL_ALPHA && p->Alpha.Opcode == RC_OPCODE_NOP) {
      *why = "RGB.REPL_ALPHA (SOP) reads a NOP alpha lane";
      return false;
   }
   return true;
}

/* RC_OPCODE_DP3/RC_OPCODE_DP4 in the Alpha slot is the R300_ALU_OUTA_DP4
 * (r300_fragprog_emit.c translate_alpha_opcode) / R500_ALPHA_OP_DP
 * (r500_fragprog_emit.c translate_alpha_op) hardware encoding: the Alpha
 * pipe's DP mode broadcasts the RGB pipe's dot-product result rather than
 * computing an independent dot from its own operand fields.
 * radeon_pair_translate.c's classify_instruction sets needrgb and needalpha
 * together off the same source instruction for DP3(.w-written)/DP4, so a
 * legally translated pair with Alpha in DP mode always carries the matching
 * RGB dot opcode -- the SAME opcode (DP3 broadcasts a DP3, DP4 a DP4), not
 * merely some dot product.  set_pair_instruction allocates the RGB and Alpha
 * source slots for a shared instruction operand through two independent
 * rc_pair_alloc_source calls (one per pipe), so RGB.Arg[i].Source and
 * Alpha.Arg[i].Source can land on different slot numbers in their own Src[]
 * arrays; what must agree is the register each slot resolves to, since Alpha
 * broadcasts the value RGB's operands compute. */
static bool
check_alpha_reads_rgb_dot(const struct rc_pair_instruction *p, const char **why)
{
   const bool alpha_is_dp =
      p->Alpha.Opcode == RC_OPCODE_DP3 || p->Alpha.Opcode == RC_OPCODE_DP4;
   if (!alpha_is_dp)
      return true;

   if (p->RGB.Opcode != p->Alpha.Opcode) {
      *why = "Alpha.DP does not match the RGB lane's dot-product opcode";
      return false;
   }

   const struct rc_opcode_info *info = rc_get_opcode_info(p->RGB.Opcode);
   for (unsigned i = 0; i < info->NumSrcRegs && i < 3; i++) {
      const struct rc_pair_instruction_source *rgb_src = &p->RGB.Src[p->RGB.Arg[i].Source];
      const struct rc_pair_instruction_source *alpha_src = &p->Alpha.Src[p->Alpha.Arg[i].Source];
      if (!rgb_src->Used && !alpha_src->Used)
         continue;
      if (rgb_src->File != alpha_src->File || rgb_src->Index != alpha_src->Index) {
         *why = "Alpha.DP reads a different source than the RGB dot product it broadcasts";
         return false;
      }
   }
   return true;
}

/* WriteALUResult and an output write share encoding bits on both targets:
 * r500_fragprog_emit.c's emit_paired refuses "Cannot write output and ALU
 * result at the same time" for RGB.OutputWriteMask, Alpha.OutputWriteMask,
 * OR Alpha.DepthWriteMask together with WriteALUResult, and
 * radeon_pair_schedule.c's merge_instructions enforces the same rule before
 * merging two half-full pairs.  A pair that reaches final validation with
 * both set slipped past that guard. */
static bool
check_no_double_issue(const struct rc_pair_instruction *p, const char **why)
{
   if (p->WriteALUResult &&
       (p->RGB.OutputWriteMask || p->Alpha.OutputWriteMask || p->Alpha.DepthWriteMask)) {
      *why = "WriteALUResult and an output write are both set on one pair";
      return false;
   }
   return true;
}

/* Mirrors radeon_pair_schedule.c's presub_nop(): a presubtract source that
 * reads one of the given (RGB, Alpha) destination indices from the
 * immediately preceding pair is the presub-NOP hazard. */
static bool
presub_reads_index(const struct rc_pair_sub_instruction *sub, int prev_rgb_index,
                   int prev_alpha_index)
{
   if (!sub->Src[RC_PAIR_PRESUB_SRC].Used)
      return false;

   const unsigned num_src =
      rc_presubtract_src_reg_count(sub->Src[RC_PAIR_PRESUB_SRC].Index);
   for (unsigned i = 0; i < num_src; i++) {
      if (sub->Src[i].File == RC_FILE_TEMPORARY &&
          ((int)sub->Src[i].Index == prev_rgb_index ||
           (int)sub->Src[i].Index == prev_alpha_index)) {
         return true;
      }
   }
   return false;
}

/* US_ALU_RGB_INST bit 31 is a documented NOP-insertion bit for the
 * presubtract-source hazard: when an instruction's presubtract source
 * reads the immediately preceding pair's destination, that preceding pair
 * must carry the NOP bubble.  This re-derives the same adjacency check
 * radeon_pair_schedule.c's presub_nop() uses to set the bit, and confirms
 * the bit landed, rather than trusting the scheduler that set it. */
static bool
check_presub_hazard_nop(const struct rc_instruction *prev, const struct rc_instruction *cur,
                        const char **why)
{
   if (!prev || prev->Type != RC_INSTRUCTION_PAIR)
      return true;

   const int prev_rgb_index = prev->U.P.RGB.WriteMask ? (int)prev->U.P.RGB.DestIndex : -1;
   const int prev_alpha_index = prev->U.P.Alpha.WriteMask ? (int)prev->U.P.Alpha.DestIndex : -1;

   const bool hazard =
      presub_reads_index(&cur->U.P.RGB, prev_rgb_index, prev_alpha_index) ||
      presub_reads_index(&cur->U.P.Alpha, prev_rgb_index, prev_alpha_index);

   if (hazard && !prev->U.P.Nop) {
      *why = "presubtract reads the immediately preceding pair's destination "
             "without a NOP bubble";
      return false;
   }
   return true;
}

bool
r300_classic_validate_schedule(struct radeon_compiler *c, const char **reject_reason)
{
   /* reject_reason is an optional out-parameter; every check_* helper writes
    * through a non-NULL pointer unconditionally, so route them through a
    * local that always exists and only copy out to the caller's pointer,
    * if any, at the point of rejection.  A caller that does pass a pointer
    * sees it cleared here rather than holding a stale reason from an
    * earlier, unrelated call. */
   if (reject_reason)
      *reject_reason = NULL;

   const char *local_reason = NULL;
   const struct rc_instruction *prev = NULL;

   for (const struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->Type == RC_INSTRUCTION_PAIR) {
         const bool legal = check_rgb_reads_alpha(&inst->U.P, &local_reason) &&
                            check_alpha_reads_rgb_dot(&inst->U.P, &local_reason) &&
                            check_no_double_issue(&inst->U.P, &local_reason) &&
                            check_presub_hazard_nop(prev, inst, &local_reason);
         if (!legal) {
            if (reject_reason)
               *reject_reason = local_reason;
            return false;
         }
      }
      prev = inst;
   }
   return true;
}
