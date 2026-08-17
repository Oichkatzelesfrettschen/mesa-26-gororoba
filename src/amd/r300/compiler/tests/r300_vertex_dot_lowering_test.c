/*
 * SPDX-License-Identifier: MIT
 *
 * Value-parity guard for the r300..r500 vertex-engine dot lowerings
 * (radeon_program_alu.c, dispatched from r300_transform_vertex_alu).
 *
 * The vertex engine's only dot instruction is the four-term DP4, so the shorter
 * dots are widened to DP4 with the surplus terms neutralized:
 *
 *   transform_r300_vertex_DP2: DP2 -> transform_DP2 (-> DP3, Z/W swizzle ZERO,
 *       Z/W negate cleared) -> opcode patched to DP4.  Net: a DP4 whose Z AND W
 *       terms are 0*0, so the value is the true two-term dot.
 *   transform_r300_vertex_DP3: DP3 -> DP4 with the W swizzle forced to ZERO and
 *       the W negate cleared.  Net: a DP4 whose W term is 0*0, so the value is
 *       the true three-term dot.
 *
 * Neither had a test -- there was no vertex-ALU-transform test at all.  A drift
 * that left a surplus term un-neutralized (or dropped a negate clear) would
 * silently turn dot2/dot3 into a longer dot.  This guard is the vertex sibling of
 * r300_dp2_lowering_test.c (the fragment DP2 -> DP3 guard).
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

/* Build one dot instruction (opcode DP2 or DP3) of TEMP[1] . TEMP[2] with full
 * XYZW swizzles and a surplus-term negate on src0, run the vertex ALU rewrite,
 * and return the widened DP4 (the program's sole remaining instruction). */
static struct rc_instruction *
build_and_lower(struct radeon_compiler *c, rc_opcode dot, unsigned surplus_negate)
{
   struct rc_instruction *inst =
      rc_insert_new_instruction(c, c->Program.Instructions.Prev);
   inst->U.I.Opcode           = dot;
   inst->U.I.DstReg.File      = RC_FILE_TEMPORARY;
   inst->U.I.DstReg.Index     = 0;
   inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
   inst->U.I.SrcReg[0].File    = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[0].Index   = 1;
   inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
   inst->U.I.SrcReg[0].Negate  = surplus_negate;
   inst->U.I.SrcReg[1].File    = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[1].Index   = 2;
   inst->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_XYZW;
   inst->U.I.SrcReg[1].Negate  = 0;

   const int handled = r300_transform_vertex_alu(c, inst, NULL);
   CHECK(handled == 1, dot == RC_OPCODE_DP2 ? "vertex ALU rewrite claims the DP2"
                                            : "vertex ALU rewrite claims the DP3");
   struct rc_instruction *out = c->Program.Instructions.Next;
   CHECK(out != &c->Program.Instructions && out->Next == &c->Program.Instructions,
         "exactly one instruction remains");
   return out;
}

/* TEMP inputs: Z and W are large so a surviving surplus term swamps the dot. */
static const float temps[3][4] = {
   {0, 0, 0, 0},          /* TEMP[0] dst, unused as a source */
   {2, 3, 100, 200},      /* TEMP[1] */
   {5, 7, 300, 400},      /* TEMP[2] */
};

static float
eval_dp4(const struct rc_instruction *dp4)
{
   float acc = 0.0f;
   for (unsigned ch = 0; ch < 4; ch++)
      acc += resolve_chan(&dp4->U.I.SrcReg[0], ch, temps) *
             resolve_chan(&dp4->U.I.SrcReg[1], ch, temps);
   return acc;
}

int
main(void)
{
   struct rc_regalloc_state rs;
   struct radeon_compiler c;
   rc_init_regalloc_state(&rs, RC_VERTEX_PROGRAM);
   rc_init(&c, &rs);
   c.type = RC_VERTEX_PROGRAM;

   printf("r300 vertex dot lowering (DP2/DP3 -> DP4) value parity:\n");

   /* DP2 -> DP4: both Z and W terms neutralized; src0 carried a Z/W negate. */
   struct rc_instruction *dp4a = build_and_lower(&c, RC_OPCODE_DP2,
                                                 RC_MASK_Z | RC_MASK_W);
   const struct rc_src_register *a0 = &dp4a->U.I.SrcReg[0];
   const struct rc_src_register *a1 = &dp4a->U.I.SrcReg[1];
   CHECK(dp4a->U.I.Opcode == RC_OPCODE_DP4, "DP2 widened to DP4");
   CHECK(GET_SWZ(a0->Swizzle, 0) == RC_SWIZZLE_X &&
         GET_SWZ(a0->Swizzle, 1) == RC_SWIZZLE_Y, "DP2->DP4 src0 X/Y preserved");
   CHECK(GET_SWZ(a0->Swizzle, 2) == RC_SWIZZLE_ZERO &&
         GET_SWZ(a0->Swizzle, 3) == RC_SWIZZLE_ZERO, "DP2->DP4 src0 Z/W zeroed");
   CHECK(GET_SWZ(a1->Swizzle, 2) == RC_SWIZZLE_ZERO &&
         GET_SWZ(a1->Swizzle, 3) == RC_SWIZZLE_ZERO, "DP2->DP4 src1 Z/W zeroed");
   CHECK((a0->Negate & (RC_MASK_Z | RC_MASK_W)) == 0, "DP2->DP4 src0 Z/W negate cleared");
   const float dot2 = temps[1][0] * temps[2][0] + temps[1][1] * temps[2][1];
   CHECK(eval_dp4(dp4a) == dot2, "DP2->DP4 evaluates to the two-term dot");

   /* Reset the program for the DP3 case. */
   while (c.Program.Instructions.Next != &c.Program.Instructions)
      rc_remove_instruction(c.Program.Instructions.Next);

   /* DP3 -> DP4: only the W term neutralized; src0 carried a W negate. */
   struct rc_instruction *dp4b = build_and_lower(&c, RC_OPCODE_DP3, RC_MASK_W);
   const struct rc_src_register *b0 = &dp4b->U.I.SrcReg[0];
   const struct rc_src_register *b1 = &dp4b->U.I.SrcReg[1];
   CHECK(dp4b->U.I.Opcode == RC_OPCODE_DP4, "DP3 widened to DP4");
   CHECK(GET_SWZ(b0->Swizzle, 0) == RC_SWIZZLE_X &&
         GET_SWZ(b0->Swizzle, 1) == RC_SWIZZLE_Y &&
         GET_SWZ(b0->Swizzle, 2) == RC_SWIZZLE_Z, "DP3->DP4 src0 X/Y/Z preserved");
   CHECK(GET_SWZ(b0->Swizzle, 3) == RC_SWIZZLE_ZERO, "DP3->DP4 src0 W zeroed");
   CHECK(GET_SWZ(b1->Swizzle, 3) == RC_SWIZZLE_ZERO, "DP3->DP4 src1 W zeroed");
   CHECK((b0->Negate & RC_MASK_W) == 0, "DP3->DP4 src0 W negate cleared");
   const float dot3 = temps[1][0] * temps[2][0] + temps[1][1] * temps[2][1] +
                      temps[1][2] * temps[2][2];
   CHECK(eval_dp4(dp4b) == dot3, "DP3->DP4 evaluates to the three-term dot");
   printf("  info - dot2=%.0f dot3=%.0f (W term 200*400=80000 excluded from dot3)\n",
          dot2, dot3);

   rc_destroy(&c);
   rc_destroy_regalloc_state(&rs);
   printf("%s\n", g_failures ? "FAILED" : "PASSED");
   return g_failures ? 1 : 0;
}
