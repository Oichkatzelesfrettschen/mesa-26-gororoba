/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>

#include "nir.h"
#include "nir_builder.h"
#include "r300_r2vb.h"

static unsigned failures;

#define CHECK(condition, name)              \
   do {                                     \
      if (condition) {                      \
         printf("  ok   - %s\n", (name));  \
      } else {                              \
         printf("  FAIL - %s\n", (name)); \
         failures++;                        \
      }                                     \
   } while (0)

enum ubo_block_kind {
   UBO_BLOCK_ZERO,
   UBO_BLOCK_ONE,
   UBO_BLOCK_DYNAMIC,
};

static nir_shader *
build_mvp_shader(enum ubo_block_kind block_kind, bool constant_offset_chain,
                 bool deref_matrix, unsigned deref_binding,
                 unsigned deref_driver_location, bool rootless_deref)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_r2vb_mvp_match");

   nir_variable *position_in = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "position");
   position_in->data.location = VERT_ATTRIB_GENERIC0;
   position_in->data.driver_location = 0;

   nir_variable *position_out = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   position_out->data.location = VARYING_SLOT_POS;
   position_out->data.driver_location = 0;

   nir_variable *matrix = nir_variable_create(
      b.shader, nir_var_mem_ubo,
      glsl_array_type(glsl_vec4_type(), 4, 0), "matrix");
   matrix->data.binding = deref_binding;
   matrix->data.driver_location = deref_driver_location;

   nir_def *block;
   if (block_kind == UBO_BLOCK_DYNAMIC) {
      nir_variable *block_in = nir_variable_create(
         b.shader, nir_var_shader_in, glsl_uint_type(), "block");
      block_in->data.location = VERT_ATTRIB_GENERIC1;
      block_in->data.driver_location = 1;
      block = nir_load_var(&b, block_in);
   } else {
      block = nir_imm_int(&b, block_kind == UBO_BLOCK_ONE ? 1 : 0);
   }

   nir_def *offset;
   if (constant_offset_chain) {
      bool fold_alu = b.constant_fold_alu;
      b.constant_fold_alu = false;
      offset = nir_iadd(&b, nir_imul(&b, nir_imm_int(&b, 2),
                                    nir_imm_int(&b, 8)),
                        nir_imm_int(&b, 0));
      b.constant_fold_alu = fold_alu;
   } else {
      offset = nir_imm_int(&b, 0);
   }

   nir_def *position = nir_load_var(&b, position_in);
   nir_def *deref_matrix_value = NULL;
   if (deref_matrix) {
      nir_deref_instr *matrix_deref;
      if (rootless_deref) {
         matrix_deref = nir_build_deref_cast(
            &b, nir_imm_int64(&b, 0), nir_var_mem_ubo, glsl_vec4_type(), 16);
      } else {
         matrix_deref = nir_build_deref_var(&b, matrix);
         matrix_deref = nir_build_deref_array_imm(&b, matrix_deref, 0);
      }
      deref_matrix_value = nir_load_deref(&b, matrix_deref);
   }
   nir_def *products[4];
   for (unsigned component = 0; component < 4; component++) {
      nir_def *matrix_value = deref_matrix
                                 ? nir_channel(&b, deref_matrix_value, component)
                                 : nir_load_ubo(
                                      &b, 1, 32, block, offset,
                                      .align_mul = 4, .align_offset = 0,
                                      .range = 4);
      products[component] = nir_fmul(
         &b, nir_channel(&b, position, component), matrix_value);
   }
   nir_def *sum = nir_fadd(
      &b, nir_fadd(&b, products[0], products[1]),
      nir_fadd(&b, products[2], products[3]));
   nir_store_var(&b, position_out, nir_replicate(&b, sum, 4), 0xf);

   nir_validate_shader(b.shader, "r300 R2VB MVP matcher input");
   return b.shader;
}

static unsigned
count_alu_op(nir_shader *shader, nir_op op)
{
   unsigned count = 0;
   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               count++;
         }
      }
   }
   return count;
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   printf("r300 R2VB MVP UBO matcher\n");

   nir_shader *block_zero =
      build_mvp_shader(UBO_BLOCK_ZERO, false, false, 0, 0, false);
   CHECK(r300_r2vb_nir_is_mvp(block_zero), "constant UBO block zero admits");
   ralloc_free(block_zero);

   nir_shader *block_one =
      build_mvp_shader(UBO_BLOCK_ONE, false, false, 0, 0, false);
   CHECK(!r300_r2vb_nir_is_mvp(block_one), "constant UBO block one rejects");
   ralloc_free(block_one);

   nir_shader *dynamic_block =
      build_mvp_shader(UBO_BLOCK_DYNAMIC, false, false, 0, 0, false);
   CHECK(!r300_r2vb_nir_is_mvp(dynamic_block), "dynamic UBO block rejects");
   ralloc_free(dynamic_block);

   nir_shader *constant_chain =
      build_mvp_shader(UBO_BLOCK_ZERO, true, false, 0, 0, false);
   CHECK(count_alu_op(constant_chain, nir_op_iadd) == 1 &&
            count_alu_op(constant_chain, nir_op_imul) == 1,
         "constant offset chain exists before classification");
   CHECK(r300_r2vb_nir_is_mvp(constant_chain),
         "constant offset chain admits after clone folding");
   CHECK(count_alu_op(constant_chain, nir_op_iadd) == 1 &&
            count_alu_op(constant_chain, nir_op_imul) == 1,
         "classification leaves source NIR unchanged");
   ralloc_free(constant_chain);

   nir_shader *deref_api_binding_nonzero =
      build_mvp_shader(UBO_BLOCK_ZERO, false, true, 7, 0, false);
   CHECK(r300_r2vb_nir_is_mvp(deref_api_binding_nonzero),
         "dereferenced UBO driver slot zero admits with API binding seven");
   ralloc_free(deref_api_binding_nonzero);

   nir_shader *deref_driver_slot_one =
      build_mvp_shader(UBO_BLOCK_ZERO, false, true, 0, 1, false);
   CHECK(!r300_r2vb_nir_is_mvp(deref_driver_slot_one),
         "dereferenced UBO driver slot one rejects with API binding zero");
   ralloc_free(deref_driver_slot_one);

   nir_shader *rootless_deref =
      build_mvp_shader(UBO_BLOCK_ZERO, false, true, 0, 0, true);
   CHECK(!r300_r2vb_nir_is_mvp(rootless_deref),
         "dereference cast without a variable root rejects");
   ralloc_free(rootless_deref);

   glsl_type_singleton_decref();
   printf("%s\n", failures ? "FAILED" : "PASSED");
   return failures ? 1 : 0;
}
