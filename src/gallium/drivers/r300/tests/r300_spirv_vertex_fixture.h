/*
 * SPDX-License-Identifier: MIT
 *
 * Test-owned SPIR-V-derived vertex front end for the r300g R2VB planner
 * oracles: a corpus module lowers to the vertex-shader NIR shape the
 * planner consumes, and a corpus manifest names every retained module so
 * a census proves its row table complete.
 */

#ifndef R300_SPIRV_VERTEX_FIXTURE_H
#define R300_SPIRV_VERTEX_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nir.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"

#include "typed_carry_corpus/r300_typed_carry_reference.h"

/* The corpus modules declare a 128-byte constant window (the Vulkan
 * push-constant block); r300 has one float constant file per stage, so the
 * window lowers onto uniform block 0 exactly as a GL uniform block does. */
#define R300_SPIRV_FIXTURE_CONSTANT_WINDOW_BYTES 128u

/* Declare a sized uniform block 0: the compiler sizes RC constants from the
 * nir_var_mem_ubo interface declaration, and load_ubo alone carries no
 * size. */
static inline void
r300_spirv_fixture_declare_block0_ubo(nir_shader *nir, unsigned size_bytes)
{
   const struct glsl_type *ubo_type =
      glsl_array_type(glsl_vec4_type(), DIV_ROUND_UP(size_bytes, 16), 16);
   struct glsl_struct_field field = {
      .type = ubo_type,
      .name = "data",
      .location = -1,
   };
   nir_variable *ubo = nir_variable_create(nir, nir_var_mem_ubo, ubo_type,
                                           "block0_ubo");
   ubo->data.driver_location = 0;
   ubo->data.binding = 0;
   ubo->data.explicit_binding = 1;
   ubo->interface_type = glsl_interface_type(
      &field, 1, GLSL_INTERFACE_PACKING_STD430, false, "__block0_ubo");
   nir->info.num_ubos = MAX2(nir->info.num_ubos, 1);
   nir->info.first_ubo_is_default_ubo = true;
}

/* load_push_constant(offset) -> load_ubo(block 0, BASE + offset), so the
 * window flows through nir_lower_ubo_vec4 like any block-0 uniform. */
static inline void
r300_spirv_fixture_lower_constant_window_to_block0(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_push_constant)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_def *off = intr->src[0].ssa;
            unsigned base = nir_intrinsic_base(intr);
            if (base)
               off = nir_iadd_imm(&b, off, base);
            nir_def *val = nir_load_ubo(
               &b, intr->def.num_components, intr->def.bit_size,
               nir_imm_int(&b, 0), off,
               .align_mul = nir_intrinsic_align_mul(intr),
               .align_offset = nir_intrinsic_align_offset(intr),
               .range_base = 0,
               .range = R300_SPIRV_FIXTURE_CONSTANT_WINDOW_BYTES);
            nir_def_rewrite_uses(&intr->def, val);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   r300_spirv_fixture_declare_block0_ubo(
      nir, R300_SPIRV_FIXTURE_CONSTANT_WINDOW_BYTES);
}

/* Every block load names literal block 0: nir_lower_explicit_io rebuilds
 * the index as a vec construct, and nir_to_rc asserts a literal 0. */
static inline void
r300_spirv_fixture_pin_block_index_zero(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_ubo &&
                intr->intrinsic != nir_intrinsic_load_ubo_vec4)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&intr->src[0], nir_imm_int(&b, 0));
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
}

/* Dense attribute slots: the draw path reads driver_location as the AOS
 * row index, and the state tracker assigns it before create_vs_state. */
static inline bool
r300_spirv_fixture_assign_input_locations(nir_shader *nir)
{
   unsigned input_span = 0;
   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.location < VERT_ATTRIB_GENERIC0)
         return false;
      const unsigned driver_location =
         var->data.location - VERT_ATTRIB_GENERIC0;
      const unsigned slots = glsl_count_attribute_slots(var->type, false);
      var->data.driver_location = driver_location;
      input_span = MAX2(input_span, driver_location + slots);
   }
   nir->num_inputs = input_span;
   return true;
}

/* One corpus module to the vertex-shader NIR the planner consumes:
 * spirv_to_nir with the r300 vertex options (Vulkan environment, 32-bit
 * index/offset buffer addressing, the caller's SW-TCL NIR options), the
 * neutral post-SPIR-V normalization (initializers, returns, inlining,
 * entry-point pruning, struct splitting, dead interface variables), the
 * constant window on uniform block 0 as load_ubo_vec4 with a literal block
 * index, dense attribute slots, assigned outputs. */
static inline nir_shader *
r300_spirv_fixture_prepare_vertex(
   const uint32_t *words, size_t size_bytes,
   const struct nir_shader_compiler_options *nir_options)
{
   static const struct spirv_capabilities capabilities = {
      .Matrix = true,
      .Shader = true,
   };
   const struct spirv_to_nir_options options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &capabilities,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
      .skip_os_break_in_debug_build = true,
   };
   nir_shader *nir =
      spirv_to_nir(words, size_bytes / 4, NULL, MESA_SHADER_VERTEX, "main",
                   &options, nir_options);
   if (!nir)
      return NULL;
   nir_validate_shader(nir, "after spirv_to_nir");

   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value,
            NULL);
   NIR_PASS(_, nir, nir_propagate_invariant, false);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_function_temp | nir_var_shader_out, UINT32_MAX);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   r300_spirv_fixture_lower_constant_window_to_block0(nir);
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_ubo_vec4);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   r300_spirv_fixture_pin_block_index_zero(nir);
   if (!r300_spirv_fixture_assign_input_locations(nir)) {
      ralloc_free(nir);
      return NULL;
   }
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   return nir;
}

/* The retained typed-carry corpus, one entry per module the header
 * embeds.  A census row table proves completeness against this manifest:
 * every entry has exactly one row, and every SPIR-V row names an entry. */
struct r300_spirv_fixture_corpus_entry {
   const char *name;
   const uint32_t *spirv;
   size_t spirv_size;
};

#define R300_SPIRV_FIXTURE_CORPUS_ENTRY(module) \
   { #module, module##_spirv, sizeof(module##_spirv) }

static const struct r300_spirv_fixture_corpus_entry
   r300_spirv_fixture_corpus[] = {
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_bool),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_bool_carry),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_sint_exact),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_uint_exact),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_sint_pos_outside),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_sint_neg_outside),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_uint_outside),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_uint_unbounded),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_sint_to_uint),
   R300_SPIRV_FIXTURE_CORPUS_ENTRY(t_uint_to_sint),
};

#endif /* R300_SPIRV_VERTEX_FIXTURE_H */
