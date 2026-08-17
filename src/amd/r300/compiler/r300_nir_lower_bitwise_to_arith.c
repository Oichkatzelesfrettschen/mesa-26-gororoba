/*
 * SPDX-License-Identifier: MIT
 */

/*
 * R300 has no native integer ALU: nir_to_rc runs nir_lower_int_to_float to
 * rewrite integer math as FP24 arithmetic.  That pass handles add/mul/div/mod
 * and comparisons, but it has no lowering for bitwise and shift ops, so they
 * fall into its default branch and assert.
 *
 * Some bitfield idioms are still representable.  FP24 RTZ holds every integer
 * with |n| <= 2^17 = 131072 exactly (r300_numeric_domain.c records this as
 * exact_int_bound = 131072), so for a value proven to stay inside that window:
 *
 *   iand(x, 2^n - 1) == x mod 2^n        ->  umod(x, 2^n)
 *   ushr(x, c)       == x / 2^c          ->  udiv(x, 2^c)
 *
 * nir_lower_int_to_float already lowers umod/udiv, so converting the admitted
 * subset here lets those shaders compile on the FP24 fragment ALU.  Everything
 * outside the proof -- a dynamic mask, a value outside the exact window (a
 * uint32 texelFetch is the canonical case), or an op with no exact arithmetic
 * form -- is genuinely unrepresentable on r300.  Rather than letting it reach
 * the int_to_float assert, replace it with a placeholder and flag the shader so
 * the caller raises rc_error; the r300 fragment translator then substitutes its
 * dummy shader.  No silent FP24 approximation is ever emitted for the
 * unrepresentable cases.
 */

#include "r300_nir.h"

#include "compiler/nir/nir_builder.h"
#include "util/hash_table.h"

/* FP24 RTZ represents integers exactly only up to this bound. */
#define R300_FP24_EXACT_INT 131072u /* 2^17 */

struct bitwise_state {
   bool unsupported;
   struct hash_table *range_ht;
};

static bool
alu_src_const_u32_components(nir_alu_instr *alu, unsigned src,
                             uint32_t values[NIR_MAX_VEC_COMPONENTS])
{
   if (!nir_src_is_const(alu->src[src].src))
      return false;

   for (unsigned component = 0; component < alu->def.num_components; component++) {
      values[component] = nir_src_comp_as_uint(
         alu->src[src].src, alu->src[src].swizzle[component]);
   }

   return true;
}

/* Each value component consumed by umod or udiv must stay inside the FP24
 * exact-integer window; int_to_float otherwise rounds and silently corrupts the
 * result.  An iand lane with mask zero emits no arithmetic and needs no bound. */
static bool
scalar_is_exact_integer(struct bitwise_state *st, nir_shader *s,
                        nir_scalar scalar)
{
   return nir_unsigned_upper_bound(s, st->range_ht, scalar) <=
          R300_FP24_EXACT_INT;
}

static bool
value_is_exact_integer(struct bitwise_state *st, nir_shader *s, nir_def *x)
{
   for (unsigned component = 0; component < x->num_components; component++) {
      if (!scalar_is_exact_integer(st, s, nir_get_scalar(x, component)))
         return false;
   }

   return true;
}

static bool
mask_covers_value_bit_size(uint32_t mask, unsigned bit_size)
{
   if (bit_size > 32)
      return false;

   return mask == (bit_size == 32 ? UINT32_MAX : (1u << bit_size) - 1);
}

static bool
alu_uses_unsupported_integer_width(const nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   nir_alu_type base_type = nir_alu_type_get_base_type(info->output_type);

   if ((base_type == nir_type_int || base_type == nir_type_uint) &&
       alu->def.bit_size != 1 && alu->def.bit_size != 32)
      return true;

   for (unsigned src = 0; src < info->num_inputs; src++) {
      base_type = nir_alu_type_get_base_type(info->input_types[src]);
      if ((base_type == nir_type_int || base_type == nir_type_uint) &&
          alu->src[src].src.ssa->bit_size != 1 &&
          alu->src[src].src.ssa->bit_size != 32)
         return true;
   }

   return false;
}

static nir_def *
lower_component_division(nir_builder *b, nir_def *value,
                         const uint32_t divisors[NIR_MAX_VEC_COMPONENTS],
                         bool use_modulus)
{
   nir_def *components[NIR_MAX_VEC_COMPONENTS];

   for (unsigned component = 0; component < value->num_components; component++) {
      nir_def *dividend = nir_channel(b, value, component);
      nir_def *divisor = nir_imm_int(b, divisors[component]);
      components[component] = use_modulus ? nir_umod(b, dividend, divisor)
                                          : nir_udiv(b, dividend, divisor);
   }

   return value->num_components == 1
             ? components[0]
             : nir_vec(b, components, value->num_components);
}

static bool
lower_bitwise_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   struct bitwise_state *st = data;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   b->cursor = nir_before_instr(instr);

   /* nir_lower_int_to_float and the RC backend accept 32-bit integer ALU
    * values. Identity and zero folds can remove a narrow iand while leaving a
    * narrow producer or constant as an integer input, so reject every
    * non-boolean integer ALU width at this admission boundary. */
   if (alu_uses_unsupported_integer_width(alu)) {
      st->unsupported = true;
      return false;
   }

   /* Boolean iand/ior/ixor/inot are logical connectives over comparison results
    * (nir_flt and friends), carried as 1-bit ops until nir_lower_bool_to_float
    * rewrites them to fmul/fmin/fmax.  They are not integer bitfield math, so
    * leave them for that pass rather than rejecting them to a dummy shader.  The
    * H.264 deblock filter's edge-strength gate -- iand(flt, iand(flt, flt)) -- is
    * exactly this shape; rejecting it substitutes a dummy deblock shader that
    * corrupts reconstructed inter macroblocks. */
   if (alu->def.bit_size == 1)
      return false;

   switch (alu->op) {
   case nir_op_iand: {
      uint32_t masks[NIR_MAX_VEC_COMPONENTS];
      uint32_t moduli[NIR_MAX_VEC_COMPONENTS];
      unsigned value_src;
      bool all_zero_mask = true;
      bool all_identity_mask = true;
      if (alu_src_const_u32_components(alu, 1, masks))
         value_src = 0;
      else if (alu_src_const_u32_components(alu, 0, masks))
         value_src = 1;
      else
         break; /* dynamic mask: no compile-time proof of a low-bit mask */

      for (unsigned component = 0; component < alu->def.num_components;
           component++) {
         /* mask 0 is exact (x & 0 == 0); non-zero masks must be 2^n-1 with a
          * modulus inside the FP24 exact-integer window. */
         if (masks[component] == 0) {
            all_identity_mask = false;
            continue;
         }
         all_zero_mask = false;
         if (mask_covers_value_bit_size(masks[component], alu->def.bit_size))
            continue;

         all_identity_mask = false;
         moduli[component] = masks[component] + 1;
         if (moduli[component] == 0 ||
             (moduli[component] & (moduli[component] - 1)) != 0 ||
             moduli[component] > R300_FP24_EXACT_INT)
            goto unsupported;
      }

      nir_def *x = nir_ssa_for_alu_src(b, alu, value_src);
      if (all_identity_mask) {
         nir_def_replace(&alu->def, x);
         return true;
      }

      if (all_zero_mask) {
         nir_def_replace(&alu->def,
                         nir_imm_zero(b, alu->def.num_components,
                                      alu->def.bit_size));
         return true;
      }

      for (unsigned component = 0; component < alu->def.num_components;
           component++) {
         if (masks[component] != 0 &&
             !mask_covers_value_bit_size(masks[component], alu->def.bit_size) &&
             !scalar_is_exact_integer(st, b->shader,
                                      nir_get_scalar(x, component)))
            goto unsupported;
      }

      nir_def *components[NIR_MAX_VEC_COMPONENTS];
      for (unsigned component = 0; component < alu->def.num_components;
           component++) {
         components[component] = masks[component] == 0
                                    ? nir_imm_zero(b, 1, alu->def.bit_size)
                                 : mask_covers_value_bit_size(
                                      masks[component], alu->def.bit_size)
                                    ? nir_channel(b, x, component)
                                    : nir_umod(
                                         b, nir_channel(b, x, component),
                                         nir_imm_intN_t(b, moduli[component],
                                                        alu->def.bit_size));
      }

      nir_def_replace(&alu->def, alu->def.num_components == 1
                                    ? components[0]
                                    : nir_vec(b, components,
                                              alu->def.num_components));
      return true;
   }
   case nir_op_ushr: {
      uint32_t shifts[NIR_MAX_VEC_COMPONENTS];
      uint32_t divisors[NIR_MAX_VEC_COMPONENTS];
      if (!alu_src_const_u32_components(alu, 1, shifts))
         break; /* variable or out-of-window shift */

      if (alu->def.bit_size != 32)
         goto unsupported;

      for (unsigned component = 0; component < alu->def.num_components;
           component++) {
         if (shifts[component] > 17)
            goto unsupported;
         divisors[component] = 1u << shifts[component];
      }

      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      if (!value_is_exact_integer(st, b->shader, x))
         break;

      nir_def_replace(&alu->def,
                      lower_component_division(b, x, divisors, false));
      return true;
   }
   case nir_op_ior:
   case nir_op_ixor:
   case nir_op_inot:
   case nir_op_ishl:
   case nir_op_ishr:
      break; /* no FP24-exact arithmetic form without a bitfield/range proof */
   default:
      return false; /* not an admitted integer bit op; leave for int_to_float */
   }

unsupported:
   /* An integer bitwise/shift op with no exact FP24 rewrite reached here.  Drop
    * it to operand 0 so int_to_float does not assert, and flag the shader; the
    * caller raises rc_error and the r300 fragment translator emits a dummy
    * shader instead of a wrong one. */
   st->unsupported = true;
   nir_def_replace(&alu->def, nir_ssa_for_alu_src(b, alu, 0));
   return true;
}

bool
r300_nir_lower_bitwise_to_arith(nir_shader *s, bool *out_unsupported)
{
   struct bitwise_state st = {
      .unsupported = false,
      .range_ht = _mesa_pointer_hash_table_create(NULL),
   };

   bool progress = nir_shader_instructions_pass(s, lower_bitwise_instr,
                                                nir_metadata_control_flow, &st);

   _mesa_hash_table_destroy(st.range_ht, NULL);
   *out_unsupported = st.unsupported;
   return progress;
}
