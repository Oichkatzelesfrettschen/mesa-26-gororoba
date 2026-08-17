/*
 * Copyright 2023 Pavel Ondračka <pavel.ondracka@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_NIR_H
#define R300_NIR_H

#include <math.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_search.h"
#include "r300_capabilities.h"
#include "r300_carrier_policy.h"
#include "radeon_code.h"

static inline bool
is_ubo_or_input(UNUSED const nir_search_state *state, const nir_alu_instr *instr, unsigned src,
                unsigned num_components, const uint8_t *swizzle)
{
   nir_instr *parent = nir_def_instr(instr->src[src].src.ssa);
   if (parent->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(parent);

   switch (intrinsic->intrinsic) {
   case nir_intrinsic_load_ubo_vec4:
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return true;
   default:
      return false;
   }
}

static inline bool
is_not_used_in_single_if(const nir_alu_instr *instr)
{
   unsigned if_uses = 0;
   nir_foreach_use (src, &instr->def) {
      if (nir_src_is_if(src))
         if_uses++;
      else
         return true;
   }
   return if_uses != 1;
}

static inline bool
is_only_used_by_intrinsic(const nir_alu_instr *instr, nir_intrinsic_op op)
{
   bool is_used = false;
   nir_foreach_use (src, &instr->def) {
      is_used = true;

      nir_instr *user_instr = nir_src_use_instr(src);
      if (user_instr->type != nir_instr_type_intrinsic)
         return false;

      const nir_intrinsic_instr *const user_intrinsic = nir_instr_as_intrinsic(user_instr);

      if (user_intrinsic->intrinsic != op)
         return false;
   }
   return is_used;
}

static inline bool
is_only_used_by_load_ubo_vec4(const nir_alu_instr *instr)
{
   return is_only_used_by_intrinsic(instr, nir_intrinsic_load_ubo_vec4);
}

static inline bool
is_only_used_by_terminate_if(const nir_alu_instr *instr)
{
   return is_only_used_by_intrinsic(instr, nir_intrinsic_terminate_if);
}

static inline bool
check_instr_and_src_value(nir_op op, nir_instr **instr, double value)
{
   if ((*instr)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(*instr);
   if (alu->op != op)
      return false;
   unsigned i;
   for (i = 0; i <= 2; i++) {
      if (i == 2) {
         return false;
      }
      nir_alu_src src = alu->src[i];
      if (nir_src_is_const(src.src)) {
         /* All components must be reading the same value. */
         for (unsigned j = 0; j < alu->def.num_components - 1; j++) {
            if (src.swizzle[j] != src.swizzle[j + 1]) {
               return false;
            }
         }
         if (fabs(nir_src_comp_as_float(src.src, src.swizzle[0]) - value) < 1e-5) {
            break;
         }
      }
   }
   *instr = nir_def_instr(alu->src[1 - i].src.ssa);
   return true;
}

static inline bool
needs_vs_trig_input_fixup(UNUSED const nir_search_state *state, const nir_alu_instr *instr, unsigned src,
                          unsigned num_components, const uint8_t *swizzle)
{
   /* We are checking for fadd(fmul(ffract(a), 2*pi), -pi) pattern
    * emitted by us and also some wined3d shaders.
    * Start with check for fadd(a, -pi).
    */
   nir_instr *parent = nir_def_instr(instr->src[src].src.ssa);
   if (!check_instr_and_src_value(nir_op_fadd, &parent, -3.141592))
      return true;
   /* Now check for fmul(a, 2 * pi). */
   if (!check_instr_and_src_value(nir_op_fmul, &parent, 6.283185))
      return true;

   /* Finally check for ffract(a). */
   if (parent->type != nir_instr_type_alu)
      return true;
   nir_alu_instr *fract = nir_instr_as_alu(parent);
   if (fract->op != nir_op_ffract)
      return true;
   return false;
}

bool r300_nir_fold_periodic_loops(nir_shader *s);

bool r300_is_only_used_as_float(const nir_alu_instr *instr);

char *r300_check_control_flow(nir_shader *s);

void r300_optimize_nir(struct nir_shader *s,
                       const struct r300_capabilities *caps);

bool r300_nir_stub_deriv(nir_shader *s);

extern bool r300_transform_vs_trig_input(struct nir_shader *shader);

extern bool r300_transform_fs_trig_input(struct nir_shader *shader);

extern bool r300_nir_fuse_fround_d3d9(struct nir_shader *shader);

extern bool r300_nir_lower_bool_to_float(struct nir_shader *shader);

extern bool r300_nir_lower_bool_to_float_fs(struct nir_shader *shader);

extern bool r300_nir_lower_ieee16_classify(struct nir_shader *shader);

extern bool r300_nir_lower_ieee16_mul(struct nir_shader *shader);

extern bool r300_nir_lower_ieee16_mul_normal_rne(struct nir_shader *shader);

extern bool r300_nir_prepare_presubtract(struct nir_shader *shader);

extern bool r300_nir_opt_algebraic_late(struct nir_shader *shader);

extern bool r300_nir_post_integer_lowering(struct nir_shader *shader);

extern bool r300_nir_lower_bitwise_to_arith(struct nir_shader *shader,
                                            bool *out_unsupported);

/* Defined in r300_vs_draw.c: on the SW-TCL vertex path, after
 * nir_lower_int_to_float, redirect the synthetic VertexIndex/InstanceIndex
 * shader_in's numeric-index consumers to an i2f32 clone while leaving the
 * raw-bit equality operands untouched.  Declared here so the r300 test suite
 * can exercise it directly. */
extern bool r300_nir_float_encode_synthetic_sysval_index_uses(struct nir_shader *nir);

extern bool r300_nir_lower_f2i_epsilon(struct nir_shader *shader);

/* Apply the fragment stage's float-to-int conversion contract selected by the
 * input-delivery mode: an interpolated fragment shader gets the smooth-varying
 * epsilon correction; a flat R2VB producer omits it.  Both r300 fragment
 * frontends route their f2i/f2u handling through this one helper so the classic
 * selector and nir_to_rc apply the same contract. */
extern bool r300_nir_apply_fs_input_semantics(struct nir_shader *shader,
                                              enum r300_fs_input_semantics semantics);

extern bool r300_nir_lower_fcsel_r500(nir_shader *shader);

extern bool r300_nir_lower_vs_alu_r300(nir_shader *shader);

extern bool r300_nir_lower_flrp(nir_shader *shader);

extern bool r300_nir_lower_comparison_fs(nir_shader *shader);

extern bool r300_nir_add_wpos(nir_shader *shader, nir_variable **wpos_var_out);

extern nir_def *r300_nir_build_carrier_pack(nir_builder *b,
                                            const struct r300_carrier_policy *policy,
                                            nir_def *value);

/* Report VS load_vertex_id/load_instance_id reads across the deref and
 * intrinsic forms that can reach the r300 NIR-to-RC path. */
extern void r300_nir_vs_reads_system_values(nir_shader *s,
                                            bool *reads_vertex_id,
                                            bool *reads_instance_id);

/* Rewrite VS load_vertex_id/load_instance_id to reads from synthetic vertex
 * inputs that the driver reserves and supplies as ordinary attributes.  A
 * negative slot leaves the corresponding intrinsic untouched so nir_to_rc can
 * reject unsupported system values deterministically. */
extern bool r300_nir_lower_vs_system_values_to_inputs(nir_shader *s,
                                                      int vertex_id_slot,
                                                      int instance_id_slot);

/* Rewrite VS gl_VertexIndex/gl_InstanceIndex to the native load_vertex_id/
 * load_instance_id intrinsics the SW draw module supplies, consuming no vertex
 * element.  Correct for non-indexed draws (any firstVertex) and base-zero
 * instancing; the synthetic-input path remains the base-inclusive default. */
extern bool r300_nir_lower_vs_system_values_to_intrinsics(nir_shader *s);

#endif /* R300_NIR_H */
