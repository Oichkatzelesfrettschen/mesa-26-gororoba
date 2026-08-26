/*
 * SPDX-License-Identifier: MIT
 */

#include "terakan_nir_wide_phi.h"

#include "terakan_env.h"

bool
terakan_nir_wide_phi_auto_segment_enabled(void)
{
   return terakan_env_gate_enabled("TERAKAN_WIDE_PHI_AUTO_SEGMENT");
}

static bool
terakan_nir_def_is_scalar_load_const(nir_def * const def)
{
   if (def == NULL || def->num_components != 1)
      return false;

   nir_instr * const instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_load_const)
      return false;

   nir_load_const_instr const * const load_const = nir_instr_as_load_const(instr);
   return load_const->def.num_components == 1;
}

static nir_def *
terakan_nir_strip_false_or(nir_def * condition)
{
   while (condition != NULL) {
      nir_instr * const instr = nir_def_instr(condition);
      if (instr->type != nir_instr_type_alu)
         break;

      nir_alu_instr * const alu = nir_instr_as_alu(instr);
      if (alu->op != nir_op_ior || alu->def.num_components != 1 || alu->def.bit_size != 1)
         break;

      if (nir_src_is_const(alu->src[0].src) && !nir_src_as_bool(alu->src[0].src)) {
         condition = alu->src[1].src.ssa;
      } else if (nir_src_is_const(alu->src[1].src) && !nir_src_as_bool(alu->src[1].src)) {
         condition = alu->src[0].src.ssa;
      } else {
         break;
      }
   }

   return condition;
}

static nir_def *
terakan_nir_case_condition_selector(nir_def * condition)
{
   condition = terakan_nir_strip_false_or(condition);
   if (condition == NULL)
      return NULL;

   nir_instr * const instr = nir_def_instr(condition);
   if (instr->type != nir_instr_type_alu)
      return NULL;

   nir_alu_instr * const alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_ieq || alu->def.num_components != 1 || alu->def.bit_size != 1)
      return NULL;

   bool const first_is_const = nir_src_is_const(alu->src[0].src);
   bool const second_is_const = nir_src_is_const(alu->src[1].src);
   if (first_is_const == second_is_const)
      return NULL;

   nir_def * const selector = first_is_const ? alu->src[1].src.ssa : alu->src[0].src.ssa;
   if (selector == NULL || selector->num_components != 1 || selector->bit_size != 32)
      return NULL;

   return selector;
}

static bool
terakan_nir_selector_is_case_modulo(nir_def * const selector, unsigned const case_count)
{
   nir_instr * const instr = nir_def_instr(selector);
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr * const alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_umod || alu->def.num_components != 1 || alu->def.bit_size != 32)
      return false;

   nir_const_value const * const divisor = nir_src_as_const_value(alu->src[1].src);
   return divisor != NULL && divisor->u32 == case_count;
}

nir_def *
terakan_nir_wide_phi_selector(nir_def * def, unsigned const case_count)
{
   if (def == NULL || case_count < 2)
      return NULL;

   nir_def * selector = NULL;
   unsigned phi_count = 0;

   while (def != NULL) {
      nir_instr * const instr = nir_def_instr(def);
      if (instr->type != nir_instr_type_phi)
         break;

      nir_phi_instr * const phi = nir_instr_as_phi(instr);
      nir_cf_node * const preceding_node = nir_cf_node_prev(&phi->instr.block->cf_node);
      if (preceding_node == NULL || preceding_node->type != nir_cf_node_if)
         return NULL;

      nir_if * const case_if = nir_cf_node_as_if(preceding_node);
      nir_phi_src * constant_source = NULL;
      nir_phi_src * previous_source = NULL;
      nir_foreach_phi_src (phi_source, phi) {
         if (phi_source->pred == nir_if_last_then_block(case_if) && constant_source == NULL) {
            constant_source = phi_source;
         } else if (phi_source->pred == nir_if_last_else_block(case_if) &&
                    previous_source == NULL) {
            previous_source = phi_source;
         } else {
            return NULL;
         }
      }

      if (constant_source == NULL || previous_source == NULL ||
          !terakan_nir_def_is_scalar_load_const(constant_source->src.ssa))
         return NULL;

      nir_def * const case_selector = terakan_nir_case_condition_selector(case_if->condition.ssa);
      if (case_selector == NULL || !terakan_nir_selector_is_case_modulo(case_selector, case_count))
         return NULL;

      if (selector == NULL)
         selector = case_selector;
      else if (selector != case_selector)
         return NULL;

      def = previous_source->src.ssa;
      ++phi_count;
   }

   return phi_count == case_count ? selector : NULL;
}
