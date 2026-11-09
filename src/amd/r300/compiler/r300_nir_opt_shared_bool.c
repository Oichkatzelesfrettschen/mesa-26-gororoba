/*
 * SPDX-License-Identifier: MIT
 *
 * Multi-output shared Boolean factoring for R300.
 *
 * Recognizes the coupled pattern:
 *   p  = a & b
 *   y0 = p | (a & c)
 *   y1 = p | (a & d)
 *
 * When p has fanout 2 and no external uses, single-expression distributive rules
 * cannot fire without duplicating p. Jointly rewriting both consumers:
 *   y0 = a & (b | c)
 *   y1 = a & (b | d)
 * removes p from the live graph and reduces total operations from 5 to 4.
 *
 * Supports both Boolean SSA representations:
 *   1) Integer Boolean: and_op = nir_op_iand, or_op = nir_op_ior
 *   2) Lowered / Tropical Float: and_op = nir_op_fmin, or_op = nir_op_fmax
 *
 * In lowered / tropical float representation, IEEE 754-2019 minimumNumber /
 * maximumNumber rules treat NaN as missing data (fmin(1, NaN) = 1,
 * fmax(NaN, 0) = 0), breaking distributive lattice axioms. If nir_fp_preserve_nan
 * or nir_fp_no_transform is present on any involved instruction, the rewrite is
 * refused (fail-closed NaN gate). When permitted, accumulated fast-math controls
 * are propagated to replacement instructions.
 *
 * If p has external uses (e.g. y2 = p), or either branch has multiple uses,
 * or if component swizzles do not match exactly, the rewrite is skipped
 * (fail-closed cost and identity gate).
 */

#include <stdlib.h>
#include <string.h>

#include "r300_nir.h"
#include "compiler/nir/nir_builder.h"

struct nir_scalar_operand {
   nir_def *ssa;
   uint8_t swizzle;
};

static inline bool
nir_scalar_operands_equal(struct nir_scalar_operand a, struct nir_scalar_operand b)
{
   return a.ssa == b.ssa && a.swizzle == b.swizzle;
}

struct bool_op_pair {
   nir_op and_op;
   nir_op or_op;
   bool is_float;
};

static const struct bool_op_pair op_pairs[] = {
   { nir_op_iand, nir_op_ior, false },
   { nir_op_fmin, nir_op_fmax, true  },
};

static bool
get_other_and_src(nir_alu_instr *and_alu, struct nir_scalar_operand match_src,
                  struct nir_scalar_operand *out_other)
{
   struct nir_scalar_operand s0 = { and_alu->src[0].src.ssa, and_alu->src[0].swizzle[0] };
   struct nir_scalar_operand s1 = { and_alu->src[1].src.ssa, and_alu->src[1].swizzle[0] };

   if (nir_scalar_operands_equal(s0, match_src)) {
      *out_other = s1;
      return true;
   }
   if (nir_scalar_operands_equal(s1, match_src)) {
      *out_other = s0;
      return true;
   }
   return false;
}

static bool
match_or_arm(nir_alu_instr *or_alu, const struct bool_op_pair *pair,
             nir_def *p_def, struct nir_scalar_operand a_op,
             struct nir_scalar_operand *out_other,
             nir_alu_instr **out_and_alu)
{
   if (or_alu->op != pair->or_op || or_alu->def.num_components != 1)
      return false;

   nir_def *s0 = or_alu->src[0].src.ssa;
   nir_def *s1 = or_alu->src[1].src.ssa;

   nir_def *other_branch = NULL;
   if (s0 == p_def && or_alu->src[0].swizzle[0] == 0)
      other_branch = s1;
   else if (s1 == p_def && or_alu->src[1].swizzle[0] == 0)
      other_branch = s0;
   else
      return false;

   nir_alu_instr *and_alu = nir_def_as_alu_or_null(other_branch);
   if (!and_alu || and_alu->op != pair->and_op || and_alu->def.num_components != 1)
      return false;

   /* Fail-closed: the paired AND branch must be single-use */
   if (!list_is_singular(&and_alu->def.uses) || nir_def_used_by_if(&and_alu->def))
      return false;

   if (out_and_alu)
      *out_and_alu = and_alu;

   return get_other_and_src(and_alu, a_op, out_other);
}

static nir_def *
build_scalar_binary(nir_builder *b, nir_op op, unsigned bit_size,
                    struct nir_scalar_operand s0,
                    struct nir_scalar_operand s1,
                    uint32_t fp_ctrl)
{
   nir_alu_instr *alu = nir_alu_instr_create(b->shader, op);
   nir_def_init(&alu->instr, &alu->def, 1, bit_size);
   alu->fp_math_ctrl = nir_op_valid_fp_math_ctrl(op, fp_ctrl);
   alu->src[0].src = nir_src_for_ssa(s0.ssa);
   alu->src[0].swizzle[0] = s0.swizzle;
   alu->src[1].src = nir_src_for_ssa(s1.ssa);
   alu->src[1].swizzle[0] = s1.swizzle;
   nir_builder_instr_insert(b, &alu->instr);
   return &alu->def;
}

bool
r300_nir_opt_shared_bool(nir_shader *s)
{
   const char *disable = getenv("R300_DISABLE_SHARED_BOOL");
   if (disable && strcmp(disable, "1") == 0)
      return false;

   bool progress = false;

   nir_foreach_function_impl(impl, s) {
      bool impl_progress = false;
      nir_builder b = nir_builder_create(impl);

      bool block_progress;
      do {
         block_progress = false;
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_alu)
                  continue;

               nir_alu_instr *p_alu = nir_instr_as_alu(instr);

               for (unsigned p_idx = 0; p_idx < ARRAY_SIZE(op_pairs); p_idx++) {
                  const struct bool_op_pair *pair = &op_pairs[p_idx];
                  if (p_alu->op != pair->and_op)
                     continue;

                  /* Fail-closed: p must be strictly scalar */
                  if (p_alu->def.num_components != 1)
                     continue;
                  if (!pair->is_float && p_alu->def.bit_size != 1 && p_alu->def.bit_size != 32)
                     continue;
                  if (pair->is_float && p_alu->def.bit_size != 32 && p_alu->def.bit_size != 16)
                     continue;

                  /* Fail-closed: p must have exactly 2 uses and no if-uses */
                  if (nir_def_used_by_if(&p_alu->def))
                     continue;
                  if (list_length(&p_alu->def.uses) != 2)
                     continue;

                  /* Get the two user instructions */
                  struct nir_src *u0_src = list_first_entry(&p_alu->def.uses, struct nir_src, use_link);
                  struct nir_src *u1_src = list_last_entry(&p_alu->def.uses, struct nir_src, use_link);

                  if (!u0_src || !u1_src || u0_src == u1_src)
                     continue;
                  if (nir_src_is_if(u0_src) || nir_src_is_if(u1_src))
                     continue;

                  nir_instr *u0_instr = nir_src_use_instr(u0_src);
                  nir_instr *u1_instr = nir_src_use_instr(u1_src);
                  if (u0_instr->type != nir_instr_type_alu ||
                      u1_instr->type != nir_instr_type_alu)
                     continue;
                  if (u0_instr == u1_instr)
                     continue;

                  nir_alu_instr *u0_alu = nir_instr_as_alu(u0_instr);
                  nir_alu_instr *u1_alu = nir_instr_as_alu(u1_instr);

                  if (u0_alu->op != pair->or_op || u1_alu->op != pair->or_op)
                     continue;
                  if (u0_alu->def.num_components != 1 || u1_alu->def.num_components != 1)
                     continue;

                  /* Try both operands of p as 'a':
                   * Case 0: a = p->src[0], b = p->src[1]
                   * Case 1: a = p->src[1], b = p->src[0]
                   */
                  struct nir_scalar_operand a_cands[2] = {
                     { p_alu->src[0].src.ssa, p_alu->src[0].swizzle[0] },
                     { p_alu->src[1].src.ssa, p_alu->src[1].swizzle[0] },
                  };
                  struct nir_scalar_operand b_cands[2] = {
                     { p_alu->src[1].src.ssa, p_alu->src[1].swizzle[0] },
                     { p_alu->src[0].src.ssa, p_alu->src[0].swizzle[0] },
                  };

                  bool matched = false;
                  for (int cand = 0; cand < 2; cand++) {
                     struct nir_scalar_operand a_op = a_cands[cand];
                     struct nir_scalar_operand b_op = b_cands[cand];
                     struct nir_scalar_operand c_op = {0};
                     struct nir_scalar_operand d_op = {0};
                     nir_alu_instr *ac_alu = NULL;
                     nir_alu_instr *ad_alu = NULL;

                     if (match_or_arm(u0_alu, pair, &p_alu->def, a_op, &c_op, &ac_alu) &&
                         match_or_arm(u1_alu, pair, &p_alu->def, a_op, &d_op, &ad_alu)) {
                        uint32_t fp_ctrl = 0;
                        if (pair->is_float) {
                           fp_ctrl = p_alu->fp_math_ctrl | u0_alu->fp_math_ctrl |
                                     u1_alu->fp_math_ctrl | ac_alu->fp_math_ctrl |
                                     ad_alu->fp_math_ctrl;

                           /* Fail-closed NaN gate: IEEE 754-2019 minimumNumber / maximumNumber
                            * does not form a distributive lattice when NaN is present:
                            *   min(1, NaN) = 1, max(NaN, 0) = 0.
                            * Refuse the rewrite if NaN preservation or exact / no-transform
                            * math is requested.
                            */
                           if (fp_ctrl & (nir_fp_preserve_nan | nir_fp_no_transform))
                              continue;
                        }

                        /* Found match! Jointly rewrite u0 and u1 */
                        unsigned bsize = p_alu->def.bit_size;

                        b.cursor = nir_before_instr(&u0_alu->instr);
                        nir_def *or_bc = build_scalar_binary(&b, pair->or_op, bsize, b_op, c_op, fp_ctrl);
                        struct nir_scalar_operand or_bc_op = { or_bc, 0 };
                        nir_def *new_u0 = build_scalar_binary(&b, pair->and_op, bsize, a_op, or_bc_op, fp_ctrl);
                        nir_def_replace(&u0_alu->def, new_u0);

                        b.cursor = nir_before_instr(&u1_alu->instr);
                        nir_def *or_bd = build_scalar_binary(&b, pair->or_op, bsize, b_op, d_op, fp_ctrl);
                        struct nir_scalar_operand or_bd_op = { or_bd, 0 };
                        nir_def *new_u1 = build_scalar_binary(&b, pair->and_op, bsize, a_op, or_bd_op, fp_ctrl);
                        nir_def_replace(&u1_alu->def, new_u1);

                        matched = true;
                        block_progress = true;
                        impl_progress = true;
                        break;
                     }
                  }

                  if (matched)
                     break;
               }
               if (block_progress)
                  break;
            }
            if (block_progress)
               break;
         }
      } while (block_progress);

      if (impl_progress) {
         nir_progress(true, impl, nir_metadata_control_flow);
         progress = true;
      }
   }

   return progress;
}
