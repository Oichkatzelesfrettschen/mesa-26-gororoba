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
 * If p has external uses (e.g. y2 = p), or either branch has multiple uses,
 * the rewrite is skipped (fail-closed cost gate).
 */

#include "r300_nir.h"
#include "compiler/nir/nir_builder.h"

static nir_def *
get_other_and_src(nir_alu_instr *and_alu, nir_def *match_src)
{
   if (and_alu->src[0].src.ssa == match_src && and_alu->src[0].swizzle[0] == 0) {
      if (and_alu->src[1].src.ssa->num_components == 1 && and_alu->src[1].swizzle[0] == 0)
         return and_alu->src[1].src.ssa;
   }
   if (and_alu->src[1].src.ssa == match_src && and_alu->src[1].swizzle[0] == 0) {
      if (and_alu->src[0].src.ssa->num_components == 1 && and_alu->src[0].swizzle[0] == 0)
         return and_alu->src[0].src.ssa;
   }
   return NULL;
}

static bool
match_ior_arm(nir_alu_instr *ior_alu, nir_def *p_def, nir_def *a_def, nir_def **out_other)
{
   if (ior_alu->op != nir_op_ior || ior_alu->def.num_components != 1)
      return false;

   if (ior_alu->src[0].swizzle[0] != 0 || ior_alu->src[1].swizzle[0] != 0)
      return false;

   nir_def *s0 = ior_alu->src[0].src.ssa;
   nir_def *s1 = ior_alu->src[1].src.ssa;

   if (s0->num_components != 1 || s1->num_components != 1)
      return false;

   nir_def *other_branch = NULL;
   if (s0 == p_def)
      other_branch = s1;
   else if (s1 == p_def)
      other_branch = s0;
   else
      return false;

   nir_alu_instr *and_alu = nir_def_as_alu_or_null(other_branch);
   if (!and_alu || and_alu->op != nir_op_iand || and_alu->def.num_components != 1)
      return false;

   if (and_alu->src[0].swizzle[0] != 0 || and_alu->src[1].swizzle[0] != 0)
      return false;

   if (and_alu->src[0].src.ssa->num_components != 1 ||
       and_alu->src[1].src.ssa->num_components != 1)
      return false;

   /* Fail-closed: the paired AND branch must be single-use */
   if (!list_is_singular(&and_alu->def.uses) || nir_def_used_by_if(&and_alu->def))
      return false;

   nir_def *other_var = get_other_and_src(and_alu, a_def);
   if (!other_var)
      return false;

   *out_other = other_var;
   return true;
}

bool
r300_nir_opt_shared_bool(nir_shader *s)
{
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
               if (p_alu->op != nir_op_iand)
                  continue;

               /* Fail-closed: p must be strictly scalar Boolean or 32-bit integer */
               if (p_alu->def.num_components != 1)
                  continue;
               if (p_alu->def.bit_size != 1 && p_alu->def.bit_size != 32)
                  continue;

               /* Fail-closed: both sources of p must be strictly scalar with swizzle 0 */
               if (p_alu->src[0].swizzle[0] != 0 || p_alu->src[1].swizzle[0] != 0)
                  continue;
               if (p_alu->src[0].src.ssa->num_components != 1 ||
                   p_alu->src[1].src.ssa->num_components != 1)
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

               if (u0_alu->op != nir_op_ior || u1_alu->op != nir_op_ior)
                  continue;
               if (u0_alu->def.num_components != 1 || u1_alu->def.num_components != 1)
                  continue;

               /* Try both operands of p as 'a':
                * Case 0: a = p->src[0], b = p->src[1]
                * Case 1: a = p->src[1], b = p->src[0]
                */
               nir_def *a_candidates[2] = { p_alu->src[0].src.ssa, p_alu->src[1].src.ssa };
               nir_def *b_candidates[2] = { p_alu->src[1].src.ssa, p_alu->src[0].src.ssa };

               bool matched = false;
               for (int cand = 0; cand < 2; cand++) {
                  nir_def *a = a_candidates[cand];
                  nir_def *b_val = b_candidates[cand];

                  nir_def *c = NULL;
                  nir_def *d = NULL;

                  if (match_ior_arm(u0_alu, &p_alu->def, a, &c) &&
                      match_ior_arm(u1_alu, &p_alu->def, a, &d)) {
                     /* Found match! Jointly rewrite u0 and u1 */
                     b.cursor = nir_before_instr(&u0_alu->instr);
                     nir_def *new_u0 = nir_iand(&b, a, nir_ior(&b, b_val, c));
                     nir_def_replace(&u0_alu->def, new_u0);

                     b.cursor = nir_before_instr(&u1_alu->instr);
                     nir_def *new_u1 = nir_iand(&b, a, nir_ior(&b, b_val, d));
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
      } while (block_progress);

      if (impl_progress) {
         nir_progress(true, impl, nir_metadata_control_flow);
         progress = true;
      }
   }

   return progress;
}
