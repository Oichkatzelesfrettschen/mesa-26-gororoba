/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Classify-only admission harness for r300_nir_classify_compute.  Builds tiny
 * MESA_SHADER_COMPUTE nir_shaders and asserts the verdict against the RS482
 * compute-as-raster substrate: a kernel that only loads, does FP24-range
 * arithmetic, and writes its buffer output is admissible (the output write
 * lowers to RB3D export); workgroup shared memory, a barrier, a general atomic,
 * a storage-image / global scatter, or FP64 arithmetic each reject
 * deterministically.  The classifier never mutates, lowers, or executes the
 * shader.
 */

#include <stdbool.h>
#include <stdio.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_compute_admission.h"

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

static nir_builder
cs_builder(const char *name)
{
   static const nir_shader_compiler_options options;
   return nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "%s",
                                         name);
}

/* Admissible: load a value, FP24-range fadd, write the buffer output. */
static nir_shader *
build_admissible(void)
{
   nir_builder b = cs_builder("cs_admit");
   nir_def *x = nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                             .align_mul = 4, .align_offset = 0, .range = 4);
   nir_def *y = nir_fadd(&b, x, nir_imm_float(&b, 7.0));
   nir_store_ssbo(&b, y, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_shared_memory(void)
{
   nir_builder b = cs_builder("cs_shared");
   b.shader->info.shared_size = 64;
   return b.shader;
}

static nir_shader *
build_barrier(void)
{
   nir_builder b = cs_builder("cs_barrier");
   nir_intrinsic_instr *bar =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_barrier);
   nir_intrinsic_set_execution_scope(bar, SCOPE_WORKGROUP);
   nir_intrinsic_set_memory_scope(bar, SCOPE_WORKGROUP);
   nir_intrinsic_set_memory_semantics(bar, NIR_MEMORY_ACQ_REL);
   nir_intrinsic_set_memory_modes(bar, nir_var_mem_shared);
   nir_builder_instr_insert(&b, &bar->instr);
   return b.shader;
}

static nir_shader *
build_general_atomic(void)
{
   nir_builder b = cs_builder("cs_atomic");
   nir_intrinsic_instr *atom =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_ssbo_atomic);
   atom->num_components = 1;
   nir_def_init(&atom->instr, &atom->def, 1, 32);
   nir_intrinsic_set_atomic_op(atom, nir_atomic_op_iadd);
   atom->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0)); /* buffer */
   atom->src[1] = nir_src_for_ssa(nir_imm_int(&b, 0)); /* offset */
   atom->src[2] = nir_src_for_ssa(nir_imm_int(&b, 1)); /* value */
   nir_builder_instr_insert(&b, &atom->instr);
   return b.shader;
}

static nir_shader *
build_fp64(void)
{
   nir_builder b = cs_builder("cs_fp64");
   nir_def *a = nir_imm_double(&b, 1.0);
   nir_def *c = nir_imm_double(&b, 2.0);
   nir_def *sum = nir_fadd(&b, a, c); /* 64-bit fadd */
   nir_store_ssbo(&b, nir_f2f32(&b, sum), nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_identity_map_f32vec4(void)
{
   nir_builder b = cs_builder("cs_identity_map_f32vec4");
   nir_def *in = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_binary_map_f32vec4(void)
{
   nir_builder b = cs_builder("cs_binary_map_f32vec4");
   nir_def *a = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *sum = nir_fadd(&b, a, c);
   nir_store_ssbo(&b, sum, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static void
prepare_detect_shader(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
   } while (progress);
}

static void
case_verdict(nir_shader *nir, bool want_admit, enum r300_compute_reject want,
             const char *label)
{
   struct r300_compute_admission a;
   r300_nir_classify_compute(nir, &a);
   printf("  (%s: %s/%s)\n", label, a.admissible ? "admit" : "reject",
          r300_compute_reject_name(a.reason));
   CHECK(a.admissible == want_admit, label);
   if (!want_admit)
      CHECK(a.reason == want, "  rejection reason matches");
   ralloc_free(nir);
}

static void
case_identity_metadata(void)
{
   nir_shader *nir = build_identity_map_f32vec4();
   struct r300_compute_admission adm;
   struct r300_compute_identity_pattern ident = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 identity-map kernel admits");
   r300_nir_detect_identity_map(nir, &ident);
   CHECK(ident.is_identity_map, "float4 identity-map shape detected");
   CHECK(ident.value_components == 4, "identity-map metadata records vec4 width");
   CHECK(ident.value_bit_size == 32, "identity-map metadata records 32-bit lanes");
   ralloc_free(nir);
}

static void
case_binary_metadata(void)
{
   nir_shader *nir = build_binary_map_f32vec4();
   struct r300_compute_admission adm;
   struct r300_compute_binary_map_pattern binmap = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 binary-map kernel admits");
   r300_nir_detect_binary_map(nir, &binmap);
   CHECK(binmap.is_binary_map, "float4 binary-map shape detected");
   CHECK(binmap.alu_op == nir_op_fadd, "binary-map metadata records fadd opcode");
   CHECK(binmap.value_components == 4, "binary-map metadata records vec4 width");
   CHECK(binmap.value_bit_size == 32, "binary-map metadata records 32-bit lanes");
   CHECK(binmap.value_is_float, "binary-map metadata records float result");
   ralloc_free(nir);
}

int
main(void)
{
   printf("r300 compute NIR admission harness\n");
   case_verdict(build_admissible(), true, R300_COMPUTE_ADMIT,
                "load + FP24 fadd + buffer store admits");
   case_verdict(build_shared_memory(), false, R300_COMPUTE_REJECT_SHARED_MEMORY,
                "workgroup shared memory rejects");
   case_verdict(build_barrier(), false, R300_COMPUTE_REJECT_BARRIER,
                "control barrier rejects");
   case_verdict(build_general_atomic(), false,
                R300_COMPUTE_REJECT_GENERAL_ATOMIC, "ssbo atomic rejects");
   case_verdict(build_fp64(), false, R300_COMPUTE_REJECT_FP64,
                "fp64 arithmetic rejects");
   case_identity_metadata();
   case_binary_metadata();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
