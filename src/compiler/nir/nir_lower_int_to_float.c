/*
 * Copyright © 2018 Intel Corporation
 * Copyright © 2019 Vasily Khoruzhick <anarsoul@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"

static bool
assert_ssa_def_is_not_int(nir_def *def, void *arg)
{
   ASSERTED BITSET_WORD *int_types = arg;
   assert(!BITSET_TEST(int_types, def->index));
   return true;
}

static bool
instr_has_only_trivial_swizzles(nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];

   for (unsigned i = 0; i < info->num_inputs; i++) {
      for (unsigned chan = 0; chan < alu->def.num_components; chan++) {
         if (alu->src[i].swizzle[chan] != chan)
            return false;
      }
   }
   return true;
}

/* Recognize the y = x - ffract(x) patterns from lowered ffloor.
 * It only works for the simple case when no swizzling is involved.
 */
static bool
check_for_lowered_ffloor(nir_alu_instr *fadd)
{
   if (!instr_has_only_trivial_swizzles(fadd))
      return false;

   nir_alu_instr *fneg = NULL;
   nir_src x;
   for (unsigned i = 0; i < 2; i++) {
      nir_alu_instr *fadd_src_alu = nir_src_as_alu(fadd->src[i].src);
      if (fadd_src_alu && fadd_src_alu->op == nir_op_fneg) {
         fneg = fadd_src_alu;
         x = fadd->src[1 - i].src;
      }
   }

   if (!fneg || !instr_has_only_trivial_swizzles(fneg))
      return false;

   nir_alu_instr *ffract = nir_src_as_alu(fneg->src[0].src);
   if (ffract && ffract->op == nir_op_ffract &&
       nir_srcs_equal(ffract->src[0].src, x) &&
       instr_has_only_trivial_swizzles(ffract))
      return true;

   return false;
}

static nir_def *
lower_int_to_float_zero(nir_builder *b, nir_def *value)
{
   return nir_imm_zero(b, value->num_components, value->bit_size);
}

static nir_def *
lower_int_to_float_one(nir_builder *b, nir_def *value)
{
   return nir_imm_floatN_t(b, 1.0, value->bit_size);
}

static nir_def *
lower_int_to_float_is_zero(nir_builder *b, nir_def *value)
{
   return nir_feq(b, value, lower_int_to_float_zero(b, value));
}

static nir_def *
lower_int_to_float_signed_div_overflows(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *int_min =
      nir_imm_floatN_t(b, u_intN_min(x->bit_size), x->bit_size);
   nir_def *minus_one = nir_imm_floatN_t(b, -1.0, y->bit_size);

   return nir_iand(b, nir_feq(b, x, int_min), nir_feq(b, y, minus_one));
}

/* Truncating integer quotient evaluated in floating point.  Signed and
 * unsigned division both truncate toward zero in the no-integer float model.
 * nir_lower_int_to_float runs after nir_opt_algebraic, so fdiv is hand-lowered
 * here when the target lowers it (frcp + fmul instead of fdiv).
 */
static nir_def *
lower_int_to_float_trunc_quotient(nir_builder *b, nir_def *x, nir_def *y,
                                  nir_def *invalid_divisor)
{
   y = nir_bcsel(b, invalid_divisor, lower_int_to_float_one(b, y), y);

   if (b->shader->options->lower_fdiv)
      return nir_ftrunc(b, nir_fmul(b, x, nir_frcp(b, y)));
   return nir_ftrunc(b, nir_fdiv(b, x, y));
}

static nir_def *
lower_int_to_float_valid_or_zero(nir_builder *b, nir_def *value,
                                 nir_def *invalid)
{
   return nir_bcsel(b, invalid, lower_int_to_float_zero(b, value), value);
}

static bool
lower_alu_instr(nir_builder *b, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];

   bool is_bool_only = alu->def.bit_size == 1;
   for (unsigned i = 0; i < info->num_inputs; i++) {
      if (alu->src[i].src.ssa->bit_size != 1)
         is_bool_only = false;
   }

   if (is_bool_only) {
      /* avoid lowering integers ops are used for booleans (ieq,ine,etc) */
      return false;
   }

   b->cursor = nir_before_instr(&alu->instr);

   /* Replacement SSA value */
   nir_def *rep = NULL;
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_bcsel:
      /* These we expect to have integers but the opcode doesn't change */
      break;

   case nir_op_b2i32:
      alu->op = nir_op_b2f32;
      break;
   case nir_op_i2f32:
      alu->op = nir_op_mov;
      break;
   case nir_op_u2f32:
      alu->op = nir_op_mov;
      break;

   case nir_op_f2i32: {
      alu->op = nir_op_ftrunc;

      /* If the source was already integer, then we did't need to truncate and
       * can switch it to a mov that can be copy-propagated away.
       */
      nir_alu_instr *src_alu = nir_src_as_alu(alu->src[0].src);
      if (src_alu) {
         switch (src_alu->op) {
         /* Check for the y = x - ffract(x) patterns from lowered ffloor. */
         case nir_op_fadd:
            if (check_for_lowered_ffloor(src_alu))
               alu->op = nir_op_mov;
            break;
         case nir_op_fround_even:
         case nir_op_fceil:
         case nir_op_ftrunc:
         case nir_op_ffloor:
            alu->op = nir_op_mov;
            break;
         default:
            break;
         }
      }
      break;
   }

   case nir_op_f2u32:
      alu->op = nir_op_ffloor;
      break;

   case nir_op_ilt:
      alu->op = nir_op_flt;
      break;
   case nir_op_ige:
      alu->op = nir_op_fge;
      break;
   case nir_op_ieq:
      alu->op = nir_op_feq;
      break;
   case nir_op_ine:
      alu->op = nir_op_fneu;
      break;
   case nir_op_ult:
      alu->op = nir_op_flt;
      break;
   case nir_op_uge:
      alu->op = nir_op_fge;
      break;

   case nir_op_iadd:
      alu->op = nir_op_fadd;
      break;
   case nir_op_isub:
      alu->op = nir_op_fsub;
      break;
   case nir_op_imul:
      alu->op = nir_op_fmul;
      break;

   case nir_op_idiv: {
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      nir_def *invalid =
         nir_ior(b, lower_int_to_float_is_zero(b, y),
                 lower_int_to_float_signed_div_overflows(b, x, y));
      rep = lower_int_to_float_valid_or_zero(
         b, lower_int_to_float_trunc_quotient(b, x, y, invalid), invalid);
      break;
   }

   case nir_op_udiv: {
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      nir_def *invalid = lower_int_to_float_is_zero(b, y);
      rep = lower_int_to_float_valid_or_zero(
         b, lower_int_to_float_trunc_quotient(b, x, y, invalid), invalid);
      break;
   }

   case nir_op_irem: {
      /* Truncated remainder r = x - y * trunc(x / y). */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      nir_def *invalid =
         nir_ior(b, lower_int_to_float_is_zero(b, y),
                 lower_int_to_float_signed_div_overflows(b, x, y));
      nir_def *q = lower_int_to_float_trunc_quotient(b, x, y, invalid);
      rep = lower_int_to_float_valid_or_zero(
         b, nir_fsub(b, x, nir_fmul(b, y, q)), invalid);
      break;
   }

   case nir_op_umod: {
      /* Truncated remainder r = x - y * trunc(x / y).  This is C-style '%',
       * carrying the sign of the dividend.  Unsigned umod coincides because
       * trunc equals floor for non-negative operands.
       */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      nir_def *invalid = lower_int_to_float_is_zero(b, y);
      nir_def *q = lower_int_to_float_trunc_quotient(b, x, y, invalid);
      rep = lower_int_to_float_valid_or_zero(
         b, nir_fsub(b, x, nir_fmul(b, y, q)), invalid);
      break;
   }

   case nir_op_imod: {
      /* Floored modulo, carrying the sign of the divisor.  Start from the
       * truncated remainder r and add y when r and y differ in sign, which is
       * exactly r * y < 0 (r == 0 yields r * y == 0, so no correction).  This
       * avoids ffloor, which the r300 fragment path (nir_to_rc) does not lower
       * once nir_lower_int_to_float has run; the truncated quotient reuses the
       * idiv ftrunc form that nir_to_rc already lowers.
       */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      nir_def *invalid =
         nir_ior(b, lower_int_to_float_is_zero(b, y),
                 lower_int_to_float_signed_div_overflows(b, x, y));
      nir_def *q = lower_int_to_float_trunc_quotient(b, x, y, invalid);
      nir_def *r = nir_fsub(b, x, nir_fmul(b, y, q));
      nir_def *zero = lower_int_to_float_zero(b, r);
      rep = nir_fadd(b, r,
                     nir_bcsel(b, nir_flt(b, nir_fmul(b, r, y), zero), y, zero));
      rep = lower_int_to_float_valid_or_zero(b, rep, invalid);
      break;
   }

   case nir_op_iabs:
      alu->op = nir_op_fabs;
      break;
   case nir_op_ineg:
      alu->op = nir_op_fneg;
      break;
   case nir_op_imax:
      alu->op = nir_op_fmax;
      break;
   case nir_op_imin:
      alu->op = nir_op_fmin;
      break;
   case nir_op_umax:
      alu->op = nir_op_fmax;
      break;
   case nir_op_umin:
      alu->op = nir_op_fmin;
      break;

   case nir_op_ball_iequal2:
      alu->op = nir_op_ball_fequal2;
      break;
   case nir_op_ball_iequal3:
      alu->op = nir_op_ball_fequal3;
      break;
   case nir_op_ball_iequal4:
      alu->op = nir_op_ball_fequal4;
      break;
   case nir_op_bany_inequal2:
      alu->op = nir_op_bany_fnequal2;
      break;
   case nir_op_bany_inequal3:
      alu->op = nir_op_bany_fnequal3;
      break;
   case nir_op_bany_inequal4:
      alu->op = nir_op_bany_fnequal4;
      break;

   case nir_op_i32csel_gt:
      alu->op = nir_op_fcsel_gt;
      break;
   case nir_op_ishl: {
      /* x << y  ==  x * 2^y.  Exact while the result stays within the float
       * mantissa, which covers the small constant-buffer indices that reach a
       * float-only path (e.g. the byte offset i*stride that nir_lower_ubo_vec4
       * feeds for a dynamically indexed UBO).  Without this the op trips the
       * int/uint assert below. */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      rep = nir_fmul(b, x, nir_fexp2(b, y));
      break;
   }

   case nir_op_ushr: {
      /* x >> y  ==  trunc(x * 2^-y) for non-negative x.  This is the byte->vec4
       * index divide (ushr by 4) nir_lower_ubo_vec4 emits for a dynamically
       * indexed UBO; the float-only SW-TCL vertex path needs it to address the
       * constant file by a runtime index.  ftrunc (not ffloor) is used so the
       * r300 fragment path's nir_to_rc, which lowers ftrunc but not ffloor, can
       * still consume the result; trunc equals floor for non-negative x. */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
      rep = nir_ftrunc(b, nir_fmul(b, x, nir_fexp2(b, nir_fneg(b, y))));
      break;
   }

   case nir_op_i32csel_ge:
      alu->op = nir_op_fcsel_ge;
      break;

   default:
      assert(nir_alu_type_get_base_type(info->output_type) != nir_type_int &&
             nir_alu_type_get_base_type(info->output_type) != nir_type_uint);
      for (unsigned i = 0; i < info->num_inputs; i++) {
         assert(nir_alu_type_get_base_type(info->input_types[i]) != nir_type_int &&
                nir_alu_type_get_base_type(info->input_types[i]) != nir_type_uint);
      }
      return false;
   }
   alu->fp_math_ctrl = nir_op_valid_fp_math_ctrl(alu->op, alu->fp_math_ctrl);

   if (rep) {
      /* We've emitted a replacement instruction */
      nir_def_replace(&alu->def, rep);
   }

   return true;
}

static bool
nir_lower_int_to_float_impl(nir_function_impl *impl)
{
   bool progress = false;
   BITSET_WORD *float_types = NULL, *int_types = NULL;

   nir_builder b = nir_builder_create(impl);

   nir_index_ssa_defs(impl);
   float_types = BITSET_CALLOC(impl->ssa_alloc);
   int_types = BITSET_CALLOC(impl->ssa_alloc);
   nir_gather_types(impl, float_types, int_types);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_alu:
            progress |= lower_alu_instr(&b, nir_instr_as_alu(instr));
            break;

         case nir_instr_type_load_const: {
            nir_load_const_instr *load = nir_instr_as_load_const(instr);
            if (load->def.bit_size != 1 && BITSET_TEST(int_types, load->def.index)) {
               for (unsigned i = 0; i < load->def.num_components; i++) {
                  load->value[i] =
                     nir_const_value_for_float(
                        nir_const_value_as_int(load->value[i],
                                               load->def.bit_size),
                        load->def.bit_size);
               }
            }
            break;
         }

         case nir_instr_type_intrinsic:
         case nir_instr_type_undef:
         case nir_instr_type_phi:
         case nir_instr_type_tex:
            break;

         default:
            nir_foreach_def(instr, assert_ssa_def_is_not_int, (void *)int_types);
            break;
         }
      }
   }

   nir_progress(progress, impl, nir_metadata_control_flow);

   free(float_types);
   free(int_types);

   return progress;
}

bool
nir_lower_int_to_float(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      if (nir_lower_int_to_float_impl(impl))
         progress = true;
   }

   return progress;
}
