/*
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
build_global_scatter(void)
{
   nir_builder b = cs_builder("cs_global_scatter");
   nir_store_global(&b, nir_imm_int(&b, 5), nir_imm_int64(&b, 0),
                    .align_mul = 4, .align_offset = 0);
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

/* Quaternion Hamilton product q1*q2 in the canonical four-dot form the QMUL
 * detector admits: each output lane is a DP4 of q1 against a sign-permutation
 * of q2.  bad_sign flips one permutation lane so the negative case exercises the
 * detector's exact-permutation check.  Channels are q2.(x,y,z,w) = (w2,x2,y2,z2)
 * in the (w,x,y,z) quaternion layout. */
static nir_shader *
build_qmul_form(bool bad_sign)
{
   nir_builder b = cs_builder("cs_qmul_f32vec4");
   nir_def *q1 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q2 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *x = nir_channel(&b, q2, 0), *y = nir_channel(&b, q2, 1);
   nir_def *z = nir_channel(&b, q2, 2), *w = nir_channel(&b, q2, 3);
   nir_def *nx = nir_fneg(&b, x), *ny = nir_fneg(&b, y);
   nir_def *nz = nir_fneg(&b, z), *nw = nir_fneg(&b, w);

   nir_def *pw = nir_vec4(&b, x, bad_sign ? y : ny, nz, nw);
   nir_def *px = nir_vec4(&b, y, x, w, nz);
   nir_def *py = nir_vec4(&b, z, nw, x, y);
   nir_def *pz = nir_vec4(&b, w, z, ny, x);
   (void)nx;

   nir_def *prod = nir_vec4(&b, nir_fdot(&b, q1, pw), nir_fdot(&b, q1, px),
                            nir_fdot(&b, q1, py), nir_fdot(&b, q1, pz));
   nir_store_ssbo(&b, prod, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

/* Quaternion rotation sandwich q*embed(v)*conj(q) in the FOLDED two-Hamilton
 * form the QROTATE detector admits -- the shape the compiler produces after it
 * folds embed(v)'s 0 into the inner permutations and conj(q)'s negate into the
 * outer permutations.  The inner permutations are the Hamilton rows applied to
 * embed(v) (channel 0 -> the constant 0, others -> v.(chan-1)); the outer
 * permutations are the Hamilton rows composed with the conjugate over q. */
static nir_shader *
build_qrotate_form(void)
{
   nir_builder b = cs_builder("cs_qrotate_f32vec4");
   nir_def *q = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *v = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *zero = nir_imm_float(&b, 0.0f);
   nir_def *vx = nir_channel(&b, v, 0), *vy = nir_channel(&b, v, 1),
           *vz = nir_channel(&b, v, 2);
   nir_def *nvx = nir_fneg(&b, vx), *nvy = nir_fneg(&b, vy), *nvz = nir_fneg(&b, vz);

   nir_def *ip0 = nir_vec4(&b, zero, nvx, nvy, nvz);
   nir_def *ip1 = nir_vec4(&b, vx, zero, vz, nvy);
   nir_def *ip2 = nir_vec4(&b, vy, nvz, zero, vx);
   nir_def *ip3 = nir_vec4(&b, vz, vy, nvx, zero);
   nir_def *t = nir_vec4(&b, nir_fdot(&b, q, ip0), nir_fdot(&b, q, ip1),
                         nir_fdot(&b, q, ip2), nir_fdot(&b, q, ip3));

   nir_def *qx = nir_channel(&b, q, 0), *qy = nir_channel(&b, q, 1),
           *qz = nir_channel(&b, q, 2), *qw = nir_channel(&b, q, 3);
   nir_def *nqy = nir_fneg(&b, qy), *nqz = nir_fneg(&b, qz), *nqw = nir_fneg(&b, qw);

   nir_def *op0 = nir_vec4(&b, qx, qy, qz, qw);
   nir_def *op1 = nir_vec4(&b, nqy, qx, nqw, qz);
   nir_def *op2 = nir_vec4(&b, nqz, qw, qx, nqy);
   nir_def *op3 = nir_vec4(&b, nqw, nqz, qy, qx);
   nir_def *out = nir_vec4(&b, nir_fdot(&b, t, op0), nir_fdot(&b, t, op1),
                           nir_fdot(&b, t, op2), nir_fdot(&b, t, op3));

   nir_store_ssbo(&b, out, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
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

static void
case_qmul_metadata(void)
{
   nir_shader *nir = build_qmul_form(false);
   struct r300_compute_admission adm;
   struct r300_compute_qmul_pattern qmul = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 qmul kernel admits");
   r300_nir_detect_qmul_pattern(nir, &qmul);
   CHECK(qmul.is_qmul, "qmul Hamilton shape detected");
   CHECK(qmul.input_a_ssbo_binding == 0, "qmul metadata records q1 binding 0");
   CHECK(qmul.input_b_ssbo_binding == 1, "qmul metadata records q2 binding 1");
   CHECK(qmul.output_ssbo_binding == 2, "qmul metadata records output binding 2");
   ralloc_free(nir);

   /* A single flipped permutation sign is a different algebra; the detector's
    * exact-permutation check must reject it so the substrate's Hamilton FS never
    * silently recomputes a kernel that meant something else. */
   nir_shader *bad = build_qmul_form(true);
   struct r300_compute_qmul_pattern bad_qmul = {0};
   prepare_detect_shader(bad);
   r300_nir_detect_qmul_pattern(bad, &bad_qmul);
   CHECK(!bad_qmul.is_qmul, "qmul rejects a wrong-sign permutation");
   ralloc_free(bad);
}

static void
case_qrotate_metadata(void)
{
   nir_shader *nir = build_qrotate_form();
   struct r300_compute_admission adm;
   struct r300_compute_qrotate_pattern qr = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 qrotate kernel admits");
   r300_nir_detect_qrotate_pattern(nir, &qr);
   CHECK(qr.is_qrotate, "qrotate sandwich shape detected");
   CHECK(qr.input_q_ssbo_binding == 0, "qrotate metadata records q binding 0");
   CHECK(qr.input_v_ssbo_binding == 1, "qrotate metadata records v binding 1");
   CHECK(qr.output_ssbo_binding == 2, "qrotate metadata records output binding 2");
   ralloc_free(nir);

   /* A single Hamilton product (no sandwich) must NOT be read as a rotation; the
    * outer match would have to find an inner Hamilton product as one operand. */
   nir_shader *plain = build_qmul_form(false);
   struct r300_compute_qrotate_pattern qr2 = {0};
   prepare_detect_shader(plain);
   r300_nir_detect_qrotate_pattern(plain, &qr2);
   CHECK(!qr2.is_qrotate, "qrotate rejects a plain Hamilton product");
   ralloc_free(plain);
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
   case_verdict(build_global_scatter(), false, R300_COMPUTE_REJECT_RW_STORAGE,
                "global scatter rejects");
   case_verdict(build_fp64(), false, R300_COMPUTE_REJECT_FP64,
                "fp64 arithmetic rejects");
   case_identity_metadata();
   case_binary_metadata();
   case_qmul_metadata();
   case_qrotate_metadata();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
