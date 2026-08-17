/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_regalloc.h"

#include "util/bitset.h"
#include "util/ralloc.h"

/* Two walks over the straight-line program.  The first records each SSA
 * value's last use position.  The second allocates: at every instruction,
 * slots whose values die here return to the pool first, then the def takes
 * the lowest free slot.  A co-issued R300/R400/R500 fragment ALU (US)
 * samples every source operand before writing the destination (see the
 * OUTC/OUTA pair in r300_fragprog_emit.c and the R300 PFS instruction
 * word layout in r300_reg.h); a dying operand's temp is therefore free for
 * the def on the same instruction.  VEC expands to a MOV sequence and is
 * excluded below because sequential MOVs can overwrite a still-needed src. */

bool
r300_classic_regalloc(void *mem_ctx, const struct r300_classic_program *p,
                      struct r300_classic_regalloc_result *result)
{
   memset(result, 0, sizeof(*result));

   const unsigned num_ssa = p->next_ssa_id;
   unsigned *last_use = rzalloc_array(mem_ctx, unsigned, num_ssa + 1);
   unsigned *temp_of_ssa = ralloc_array(mem_ctx, unsigned, num_ssa + 1);
   BITSET_WORD *in_use = rzalloc_array(
      mem_ctx, BITSET_WORD, BITSET_WORDS(p->target->max_temp_regs));
   if (!last_use || !temp_of_ssa || !in_use)
      return false;
   for (unsigned i = 0; i <= num_ssa; i++)
      temp_of_ssa[i] = R300_CLASSIC_NO_TEMP;

   unsigned pos = 0;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      for (unsigned s = 0; s < i->num_srcs; s++)
         if (i->src[s].file == R300C_FILE_SSA && i->src[s].def)
            last_use[i->src[s].def->ssa_id] = pos;
      pos++;
   }

   unsigned high_water = 0;
   pos = 0;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      /* VEC expands to a MOV sequence at emission, so the read-all-before-
       * write rule that lets a def reuse a dying operand's slot does not
       * hold for it: a later MOV in the sequence would read a source the
       * earlier MOVs already overwrote.  Keep dying VEC sources live until
       * after the def allocates, so the destination never aliases one. */
      if (i->op != R300C_OP_VEC) {
         for (unsigned s = 0; s < i->num_srcs; s++) {
            if (i->src[s].file != R300C_FILE_SSA || !i->src[s].def)
               continue;
            const unsigned id = i->src[s].def->ssa_id;
            if (last_use[id] == pos &&
                temp_of_ssa[id] != R300_CLASSIC_NO_TEMP)
               BITSET_CLEAR(in_use, temp_of_ssa[id]);
         }
      }

      /* r300_classic_op_has_def, not i->writemask: R300C_OP_EXPORT_COLOR
       * carries a nonzero writemask for its destination output channels, not
       * an SSA def, and must not consume a temp register slot. */
      if (r300_classic_op_has_def(i->op)) {
         int slot = -1;
         for (unsigned t = 0; t < p->target->max_temp_regs; t++) {
            if (!BITSET_TEST(in_use, t)) {
               slot = (int)t;
               break;
            }
         }
         if (slot < 0) {
            result->reject_reason = ralloc_asprintf(
               mem_ctx,
               "t%u: live temporaries exceed the %u-slot register file",
               i->ssa_id, p->target->max_temp_regs);
            return true;
         }
         BITSET_SET(in_use, slot);
         temp_of_ssa[i->ssa_id] = (unsigned)slot;
         if ((unsigned)slot + 1 > high_water)
            high_water = slot + 1;

         /* A value never read frees its slot at its own position, so a
          * dead def cannot poison the pool. */
         if (last_use[i->ssa_id] <= pos)
            BITSET_CLEAR(in_use, slot);
      }

      if (i->op == R300C_OP_VEC) {
         for (unsigned s = 0; s < i->num_srcs; s++) {
            if (i->src[s].file != R300C_FILE_SSA || !i->src[s].def)
               continue;
            const unsigned id = i->src[s].def->ssa_id;
            if (last_use[id] == pos &&
                temp_of_ssa[id] != R300_CLASSIC_NO_TEMP)
               BITSET_CLEAR(in_use, temp_of_ssa[id]);
         }
      }
      pos++;
   }

   result->temp_of_ssa = temp_of_ssa;
   result->num_temps = high_water;
   return true;
}
