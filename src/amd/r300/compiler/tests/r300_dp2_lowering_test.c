/*
 * SPDX-License-Identifier: MIT
 *
 * Value-parity guard for transform_DP2 (radeon_program_alu.c radeonTransformALU).
 *
 * The R300/R500 fragment ALU has no native two-term dot, so RC_OPCODE_DP2 is
 * lowered to RC_OPCODE_DP3 with the Z and W swizzles of both sources forced to
 * RC_SWIZZLE_ZERO -- the three-term dot's third term becomes 0*0, so the result
 * is the true two-term dot regardless of what the DP2 sources carried in Z/W.
 *
 * This lowering had no test.  The classic-FS pair value oracle
 * (r300_classic_pair_value_test.c) never sees DP2 because this pass runs before
 * the pair scheduler, and r300_classic_parity_test.c only evaluates opcode
 * semantics -- nothing pinned the lowering itself.  A drift that left Z/W
 * un-neutralized (or dropped the negate clear) would silently turn dot2 into a
 * three- or four-term dot, and this guard would catch it.
 */

#include <stdbool.h>
#include <stdio.h>

#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_alu.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

/* Resolve one source channel to a value from the temporary-register file,
 * honouring the swizzle (X/Y/Z/W/ZERO/ONE) and the per-channel negate. */
static float
resolve_chan(const struct rc_src_register *s, unsigned chan,
             const float temps[][4])
{
   float v;
   switch (GET_SWZ(s->Swizzle, chan)) {
   case RC_SWIZZLE_X:    v = temps[s->Index][0]; break;
   case RC_SWIZZLE_Y:    v = temps[s->Index][1]; break;
   case RC_SWIZZLE_Z:    v = temps[s->Index][2]; break;
   case RC_SWIZZLE_W:    v = temps[s->Index][3]; break;
   case RC_SWIZZLE_ONE:  v = 1.0f; break;
   default:              v = 0.0f; break;   /* RC_SWIZZLE_ZERO / unused */
   }
   if (s->Negate & (1u << chan))
      v = -v;
   return v;
}

int
main(void)
{
   struct rc_regalloc_state rs;
   struct radeon_compiler c;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   rc_init(&c, &rs);
   c.type = RC_FRAGMENT_PROGRAM;

   printf("r300 transform_DP2 (DP2 -> DP3 lowering) value parity:\n");

   /* Build one DP2: TEMP[0].xyzw = DP2(TEMP[1], TEMP[2]).  Both sources carry a
    * full XYZW swizzle, and src0 additionally negates Z/W -- so the lowering's
    * swizzle-ZERO and negate-clear on Z/W are observable.  If transform_DP2
    * failed to neutralize Z/W, the DP3's third term would pick up TEMP[1].z *
    * TEMP[2].z (or its negation) and diverge from the two-term dot. */
   struct rc_instruction *inst =
      rc_insert_new_instruction(&c, c.Program.Instructions.Prev);
   inst->U.I.Opcode        = RC_OPCODE_DP2;
   inst->U.I.DstReg.File   = RC_FILE_TEMPORARY;
   inst->U.I.DstReg.Index  = 0;
   inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
   inst->U.I.SrcReg[0].File    = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[0].Index   = 1;
   inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
   inst->U.I.SrcReg[0].Negate  = RC_MASK_Z | RC_MASK_W;
   inst->U.I.SrcReg[1].File    = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[1].Index   = 2;
   inst->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_XYZW;
   inst->U.I.SrcReg[1].Negate  = 0;

   /* Run the production ALU-native rewrite on this instruction. */
   const int handled = radeonTransformALU(&c, inst, NULL);
   CHECK(handled == 1, "radeonTransformALU claims the DP2");

   /* transform_DP2 emits the DP3 before inst and removes inst, so the DP3 is now
    * the program's sole instruction. */
   struct rc_instruction *dp3 = c.Program.Instructions.Next;
   CHECK(dp3 != &c.Program.Instructions, "a lowered instruction remains");
   CHECK(dp3->Next == &c.Program.Instructions, "exactly one instruction remains");
   CHECK(dp3->U.I.Opcode == RC_OPCODE_DP3, "DP2 lowered to DP3");

   const struct rc_src_register *s0 = &dp3->U.I.SrcReg[0];
   const struct rc_src_register *s1 = &dp3->U.I.SrcReg[1];
   CHECK(GET_SWZ(s0->Swizzle, 0) == RC_SWIZZLE_X, "src0 X preserved");
   CHECK(GET_SWZ(s0->Swizzle, 1) == RC_SWIZZLE_Y, "src0 Y preserved");
   CHECK(GET_SWZ(s0->Swizzle, 2) == RC_SWIZZLE_ZERO, "src0 Z swizzle zeroed");
   CHECK(GET_SWZ(s0->Swizzle, 3) == RC_SWIZZLE_ZERO, "src0 W swizzle zeroed");
   CHECK(GET_SWZ(s1->Swizzle, 2) == RC_SWIZZLE_ZERO, "src1 Z swizzle zeroed");
   CHECK(GET_SWZ(s1->Swizzle, 3) == RC_SWIZZLE_ZERO, "src1 W swizzle zeroed");
   CHECK((s0->Negate & (RC_MASK_Z | RC_MASK_W)) == 0, "src0 Z/W negate cleared");

   /* Value parity: evaluate the DP3 (three-term) and confirm it equals the true
    * two-term dot for concrete inputs whose Z/W are large -- so a surviving third
    * term would swamp the result. */
   const float temps[3][4] = {
      {0, 0, 0, 0},          /* TEMP[0] dst, unused as a source */
      {2, 3, 100, 200},      /* TEMP[1] */
      {5, 7, 300, 400},      /* TEMP[2] */
   };
   float dp3_val = 0.0f;
   for (unsigned ch = 0; ch < 3; ch++)
      dp3_val += resolve_chan(s0, ch, temps) * resolve_chan(s1, ch, temps);
   const float dot2 = temps[1][0] * temps[2][0] + temps[1][1] * temps[2][1];
   CHECK(dp3_val == dot2, "lowered DP3 evaluates to the two-term dot");
   printf("  info - DP3=%.1f dot2=%.1f (Z/W = 100*300+200*400 = %.0f excluded)\n",
          dp3_val, dot2, 100.0 * 300.0 + 200.0 * 400.0);

   rc_destroy(&c);
   rc_destroy_regalloc_state(&rs);
   printf("%s\n", g_failures ? "FAILED" : "PASSED");
   return g_failures ? 1 : 0;
}
