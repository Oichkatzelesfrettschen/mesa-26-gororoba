/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * RS48x (RS480/RS482/RS485, CHIP_RS480 family) delivers an interpolated
 * varying roughly 2^-17 of the plane's magnitude off its analytic value.
 * Empirical: measured on the SWTCL raster path against host FP32 plane
 * evaluation of the same attributes.  FP24 holds an integer exactly only
 * up to 2^17, and that plane-equation delivery error sits at the same
 * order.  A GLSL int() truncation fed from such a varying can arrive a few
 * ULPs to the wrong side of an intended integer boundary and truncate one
 * integer low: an attribute carrying -32.0 across a triangle interpolates
 * to -31.9995, and ftrunc(-31.9995) is -31, not -32.
 *
 * FP24 cannot deliver the boundary any more precisely, so nudging the
 * operand toward its own sign before the truncate trades an unrepresentable
 * exact boundary for the boundary the source program actually intended.
 * The nudge only needs to exceed the observed ~2^-17 relative delivery
 * error and stay well under 0.5 in absolute terms for the small integers
 * these conversions carry, so a relative epsilon of 2^-15 satisfies both:
 * it corrects any boundary miss of a few ULPs, and it cannot move a value
 * whose fractional part is farther than eps * |x| from an integer, so an
 * honestly fractional int() argument keeps truncating where the source
 * asked it to.
 *
 * This targets nir_op_f2i32 and nir_op_f2u32 specifically, ahead of
 * nir_lower_int_to_float (which rewrites f2i32 to nir_op_ftrunc and f2u32 to
 * nir_op_ffloor): running before that rewrite means the nudge cannot see or affect
 * a shader-written trunc()/floor(), only the compiler's own float-to-int
 * conversion lowering.
 */

#include "r300_nir.h"
#include "nir_builder.h"

#define R300_F2I_EPS_REL (1.0f / 32768.0f) /* 2^-15 */

static bool
r300_nir_nudge_f2i_instr(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_f2i32 && alu->op != nir_op_f2u32)
      return false;

   b->cursor = nir_before_instr(instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   /* x + copysign(eps * |x|, x) folds to x * (1 + eps): sign(x) * |x| = x,
    * so the away-from-zero nudge is a single multiply -- no fsign, which
    * nir_to_rc has no opcode for at this point in the lowering. */
   nir_def *nudged = nir_fmul_imm(b, x, 1.0f + R300_F2I_EPS_REL);
   nir_src_rewrite(&alu->src[0].src, nudged);
   for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++)
      alu->src[0].swizzle[c] = MIN2(c, nudged->num_components - 1);
   return true;
}

bool
r300_nir_lower_f2i_epsilon(nir_shader *s)
{
   return nir_shader_instructions_pass(s, r300_nir_nudge_f2i_instr,
                                        nir_metadata_control_flow, NULL);
}

bool
r300_nir_apply_fs_input_semantics(nir_shader *s,
                                  enum r300_fs_input_semantics semantics)
{
   if (s->info.stage != MESA_SHADER_FRAGMENT)
      return false;

   /* Flat R2VB producer inputs do not use the empirical smooth-varying
    * interpolation correction; every other fragment shader does. */
   if (semantics == R300_FS_INPUT_INTERPOLATED)
      return r300_nir_lower_f2i_epsilon(s);

   return false;
}
