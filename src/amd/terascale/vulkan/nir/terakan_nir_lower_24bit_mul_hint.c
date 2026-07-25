/*
 * SPDX-License-Identifier: MIT
 *
 * terakan_nir_lower_24bit_mul_hint.c
 *
 * Detect 32-bit integer multiplications whose operands provably fit
 * unsigned 24 bits and rewrite them to nir_op_umul24, which SFN
 * emits as the Evergreen-native MUL_UINT24 op (vec-slot, single
 * cycle) instead of MULLO_INT (t-slot, multi-cycle).
 *
 * Why: most SPIR-V shaders that index buffers or arrays do small-
 * factor integer math (gl_GlobalInvocationID * stride, base + offset,
 * push-constant scaled coordinates).  The operands trivially fit
 * 24 bits but no hint reaches the backend; the multiply lands on
 * MULLO_INT and the vec slots sit idle.
 *
 * Hardware reference: AMD Evergreen-Family Instruction Set
 * Architecture, Section "ALU Instructions", opcodes MUL_UINT24
 * (0xB5) and MULHI_UINT24 (0xB2).  The 24-bit multiplier sits on
 * each of the four vec (xyzw) ALU slots; the full-width 32-bit
 * MULLO_INT runs only on the t-slot and consumes multiple cycles.
 *
 * Mathematical correctness: nir_op_imul returns the low 32 bits of
 * a 32x32 -> 32 multiply.  nir_op_umul24 returns the low 32 bits of
 * a 24x24 -> 48 multiply.  When both operands fit unsigned 24 bits
 * (<= 0x00FFFFFFu) the products are bit-identical in the low 32
 * bits, regardless of signed-vs-unsigned interpretation, because
 * neither expression has carry-out beyond bit 47.
 *
 * Approach modeled on nir_opt_uub.c: for each nir_op_imul
 * instruction, walk both component-0 scalar inputs via
 * nir_scalar_chase_alu_src, compute nir_unsigned_upper_bound on
 * each, and rewrite to nir_op_umul24 when both bounds <= 2^24 - 1.
 */

#include "terakan_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_range_analysis.h"

#define TERAKAN_24BIT_UPPER_BOUND  0x00FFFFFFu

typedef struct {
   nir_shader * shader;
   struct hash_table * range_ht;
} lower_state;

static bool
src_fits_24bit_unsigned(lower_state * state, nir_alu_instr * alu, unsigned src_idx)
{
   nir_scalar def_scalar = nir_get_scalar(&alu->def, 0);
   nir_scalar src_scalar = nir_scalar_chase_alu_src(def_scalar, src_idx);
   uint32_t upper_bound =
      nir_unsigned_upper_bound(state->shader, state->range_ht, src_scalar);
   return upper_bound <= TERAKAN_24BIT_UPPER_BOUND;
}

static bool
lower_alu_instruction(nir_builder * b, lower_state * state, nir_alu_instr * alu)
{
   if (alu->op != nir_op_imul) {
      return false;
   }
   if (alu->def.bit_size != 32) {
      return false;
   }
   if (alu->def.num_components != 1) {
      /* nir_opt_uub asserts num_components == 1 on its ALU paths.
       * Vector imul ops are rare after SSA lowering; skip them for
       * now rather than chasing per-component scalars. */
      return false;
   }
   if (!src_fits_24bit_unsigned(state, alu, 0)) {
      return false;
   }
   if (!src_fits_24bit_unsigned(state, alu, 1)) {
      return false;
   }

   b->cursor = nir_before_instr(&alu->instr);
   nir_def * src0 = nir_ssa_for_alu_src(b, alu, 0);
   nir_def * src1 = nir_ssa_for_alu_src(b, alu, 1);
   nir_def * lowered = nir_umul24(b, src0, src1);
   nir_def_rewrite_uses(&alu->def, lowered);
   nir_instr_remove(&alu->instr);
   return true;
}

bool
terakan_nir_lower_24bit_mul_hint(nir_shader * shader)
{
   bool progress = false;
   lower_state state = {
      .shader = shader,
      .range_ht = _mesa_pointer_hash_table_create(NULL),
   };

   nir_foreach_function_impl(impl, shader) {
      nir_builder builder = nir_builder_create(impl);
      bool impl_progress = false;

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_alu) continue;
            if (lower_alu_instruction(&builder, &state, nir_instr_as_alu(instr))) {
               impl_progress = true;
            }
         }
      }

      progress |= nir_progress(impl_progress, impl, nir_metadata_control_flow);
   }

   _mesa_hash_table_destroy(state.range_ht, NULL);
   return progress;
}
