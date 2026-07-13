/*
 * SPDX-License-Identifier: MIT
 *
 * Classify-only admission harness for r300_nir_classify_compute.  Builds tiny
 * MESA_SHADER_COMPUTE nir_shaders and asserts the verdict against the RS482
 * compute-as-raster substrate: a kernel that only loads, does FP24-range
 * arithmetic, and writes its buffer output is admissible only when the SSBO
 * write uses a canonical buffer handle and coordinate offset.  Workgroup shared
 * memory, a barrier, a general atomic, arbitrary storage writes, or FP64
 * arithmetic each reject deterministically.  The classifier never mutates,
 * lowers, or executes the shader.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "compiler/glsl_types.h"

#include "r300_compute_admission.h"
#include "r300_grid_fold.h"

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

static void prepare_detect_shader(nir_shader *nir);

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
build_fp64_operand_conversion(void)
{
   nir_builder b = cs_builder("cs_fp64_operand_conversion");
   nir_def *d = nir_imm_double(&b, 1.0);
   nir_store_ssbo(&b, nir_f2f32(&b, d), nir_imm_int(&b, 0),
                  nir_imm_int(&b, 0), .write_mask = 0x1, .align_mul = 4,
                  .align_offset = 0);
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
build_global_scatter_2x32(void)
{
   nir_builder b = cs_builder("cs_global_scatter_2x32");
   nir_store_global_2x32(&b, nir_imm_int(&b, 5), nir_imm_ivec2(&b, 0, 0),
                         .write_mask = 0x1, .align_mul = 4,
                         .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_dynamic_store_binding(void)
{
   nir_builder b = cs_builder("cs_dynamic_store_binding");
   nir_def *binding = nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 0),
                                   nir_imm_int(&b, 0), .align_mul = 4,
                                   .align_offset = 0, .range = 4);
   nir_store_ssbo(&b, nir_imm_int(&b, 5), binding, nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_data_dependent_store_offset(void)
{
   nir_builder b = cs_builder("cs_data_dependent_store_offset");
   nir_def *offset = nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 0),
                                  nir_imm_int(&b, 0), .align_mul = 4,
                                  .align_offset = 0, .range = 4);
   nir_store_ssbo(&b, nir_imm_int(&b, 5), nir_imm_int(&b, 0), offset,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_lowered_shared_load(void)
{
   nir_builder b = cs_builder("cs_lowered_shared_load");
   nir_def *v = nir_load_shared(&b, 1, 32, nir_imm_int(&b, 0),
                                .align_mul = 4, .align_offset = 0);
   nir_store_ssbo(&b, v, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_lowered_shared_store(void)
{
   nir_builder b = cs_builder("cs_lowered_shared_store");
   nir_store_shared(&b, nir_imm_int(&b, 5), nir_imm_int(&b, 0),
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

/* isub with loads emitted in program order (binding 0 first = load_a,
 * binding 1 second = load_b) but the op's left operand is load_b and right is
 * load_a: isub(load_b, load_a).  This is the ba case in r300_nir_detect_binary_map.
 * The binding assignment must follow operand order (input_a = 1, input_b = 0) not
 * load-emission order (which would wrongly give input_a = 0, input_b = 1 and
 * synthesise binding_0 - binding_1 instead of binding_1 - binding_0). */
static nir_shader *
build_binary_map_isub_ba(void)
{
   nir_builder b = cs_builder("cs_binary_map_isub_ba");
   nir_def *load0 = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                                  .align_mul = 4, .align_offset = 0);
   nir_def *load1 = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                                  .align_mul = 4, .align_offset = 0);
   /* ba operand order: left = load1 (binding 1), right = load0 (binding 0) */
   nir_def *diff = nir_isub(&b, load1, load0);
   nir_store_ssbo(&b, diff, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = in[gid], but the load offset and the store offset are built by
 * two SEPARATE calls to nir_load_global_invocation_index / nir_imul, so they
 * are structurally identical but distinct SSA defs -- the same shape
 * nir_lower_explicit_io leaves behind on real kernels (see
 * offset_scalar_semantically_equal).  Detected directly, without running
 * nir_opt_cse first, so the test exercises the walker's own structural
 * matching rather than relying on CSE to have merged the chains first. */
static nir_shader *
build_identity_map_gid_separate_offset_chains(void)
{
   nir_builder b = cs_builder("cs_identity_gid_separate_chains");
   nir_def *load_off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                                nir_imm_int(&b, 4));
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), load_off,
                               .align_mul = 4, .align_offset = 0);
   nir_def *store_off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                                 nir_imm_int(&b, 4));
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), store_off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[N-1-gid] = in[gid]: a genuine index-reversal scatter.  Value equality
 * holds (the stored value is exactly the loaded def) but the store address
 * is a different function of gid than the load address, so the offset gate
 * must reject this -- admitting it would have the fullscreen-FS replay
 * silently compute out[gid] = in[gid] instead of the kernel's actual
 * reversal. */
static nir_shader *
build_identity_map_scatter_reversed(void)
{
   nir_builder b = cs_builder("cs_identity_scatter_reversed");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *load_off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), load_off,
                               .align_mul = 4, .align_offset = 0);
   nir_def *reversed = nir_isub(&b, nir_imm_int(&b, 1023), gid);
   nir_def *store_off = nir_imul(&b, reversed, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), store_off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[id.y] = in[id.x]: a transposed-component scatter.  Same op shape
 * (channel(id, k) * 4) on both sides, differing only in which vector
 * component k is selected -- the offset gate's component check
 * (a.comp == b.comp on the invocation-id leaf) must catch this even though
 * every ALU node above it matches. */
static nir_shader *
build_identity_map_scatter_transposed_component(void)
{
   nir_builder b = cs_builder("cs_identity_scatter_transposed_component");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *load_off = nir_imul(&b, nir_channel(&b, id, 0), nir_imm_int(&b, 4));
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), load_off,
                               .align_mul = 4, .align_offset = 0);
   nir_def *store_off = nir_imul(&b, nir_channel(&b, id, 1), nir_imm_int(&b, 4));
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), store_off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = a[gid] + b[gid], with all three offsets built by separate
 * nir_load_global_invocation_index / nir_imul call pairs (structurally
 * identical, SSA-distinct) -- the binary-map analog of
 * build_identity_map_gid_separate_offset_chains. */
static nir_shader *
build_binary_map_gid_separate_offset_chains(void)
{
   nir_builder b = cs_builder("cs_binary_gid_separate_chains");
   nir_def *a_off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                             nir_imm_int(&b, 4));
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), a_off,
                              .align_mul = 4, .align_offset = 0);
   nir_def *b_off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                             nir_imm_int(&b, 4));
   nir_def *c = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), b_off,
                              .align_mul = 4, .align_offset = 0);
   nir_def *sum = nir_iadd(&b, a, c);
   nir_def *store_off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                                 nir_imm_int(&b, 4));
   nir_store_ssbo(&b, sum, nir_imm_int(&b, 2), store_off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[N-1-gid] = a[gid] + b[gid]: the binary-map analog of
 * build_identity_map_scatter_reversed.  The store address is a different
 * function of gid than either load address, so the offset gate must reject
 * it on both the store-vs-a and store-vs-b comparisons. */
static nir_shader *
build_binary_map_scatter_reversed(void)
{
   nir_builder b = cs_builder("cs_binary_scatter_reversed");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *a_off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), a_off,
                              .align_mul = 4, .align_offset = 0);
   nir_def *b_off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_def *c = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), b_off,
                              .align_mul = 4, .align_offset = 0);
   nir_def *sum = nir_iadd(&b, a, c);
   nir_def *reversed = nir_isub(&b, nir_imm_int(&b, 1023), gid);
   nir_def *store_off = nir_imul(&b, reversed, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, sum, nir_imm_int(&b, 2), store_off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Two-input transcendental map: out[gid] = f(a[gid], b[gid]) for one of the
 * non-commutative binaries (fpow / fdiv).  a is binding 0 (the op's first
 * source), b binding 1 (second), output binding 2 -- the swap flag reverses the
 * operand order so the order-preserving binding capture is exercised.  comps is
 * 1 (scalar carrier) or 4 (vec4 carrier). */
static nir_shader *
build_binary_transcendental(nir_op op, bool swap, unsigned comps)
{
   nir_builder b = cs_builder("cs_binary_transcendental");
   const unsigned align = comps * 4;
   nir_def *a = nir_load_ssbo(&b, comps, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = align, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, comps, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = align, .align_offset = 0);
   nir_def *y = swap ? nir_build_alu2(&b, op, c, a)
                     : nir_build_alu2(&b, op, a, c);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = BITFIELD_MASK(comps),
                  .align_mul = align, .align_offset = 0);
   return b.shader;
}

/* Two-input bitwise map: out[gid] = a[gid] OP b[gid] for OP in {iand,ior,ixor},
 * scalar uint32 (binding 0 = a, 1 = b, 2 = out).  The ops commute, so order is
 * not tracked. */
static nir_shader *
build_bitwise_logicop(nir_op op)
{
   nir_builder b = cs_builder("cs_bitwise_logicop");
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_build_alu2(&b, op, a, c);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Logical shift by a constant: out[gid] = a[gid] OP k for OP in {ishl, ushr}.
 * binding 0 = a, binding 1 = out; the shift amount k is an immediate. */
static nir_shader *
build_shift_logical(nir_op op, uint32_t k)
{
   nir_builder b = cs_builder("cs_shift_logical");
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_build_alu2(&b, op, a, nir_imm_int(&b, k));
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Variable shift: out[gid] = a[gid] OP b[gid] for OP in {ishl, ushr} -- the
 * amount is a second load (binding 1), not a constant.  The constant-shift
 * detector (one load) leaves it unmatched; the variable-shift detector claims it
 * and routes it to the per-element 2^b lookup carrier. */
static nir_shader *
build_shift_variable(nir_op op)
{
   nir_builder b = cs_builder("cs_shift_variable");
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *amt = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                                .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_build_alu2(&b, op, a, amt);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* QFMUL: out[gid] = a[gid] * s, a per-element vec4 quaternion (binding 1) times a
 * BROADCAST scalar s (binding 0, a one-float buffer).  nir_fmul broadcasts the
 * 1-component scalar across the vec4, so the fmul's scalar source carries a
 * 1-component def with the splat swizzle (.xxxx) the detector keys on.  The width
 * asymmetry -- a 4-component quaternion against a 1-component scalar -- is exactly
 * what separates QFMUL from the equal-width elementwise binary map. */
static nir_shader *
build_qfmul_variant(unsigned bit_size, bool scalar_per_element_offset,
                    uint32_t scalar_offset)
{
   nir_builder b = cs_builder("cs_qfmul_f32vec4");
   const unsigned scalar_bytes = bit_size / 8;
   const unsigned quat_bytes = 4 * scalar_bytes;
   nir_def *scalar_offset_def = NULL;
   if (scalar_per_element_offset) {
      scalar_offset_def = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                                   nir_imm_int(&b, (int)scalar_bytes));
   } else {
      scalar_offset_def = nir_imm_int(&b, (int)scalar_offset);
   }
   nir_def *s = nir_load_ssbo(&b, 1, bit_size, nir_imm_int(&b, 0),
                              scalar_offset_def, .align_mul = scalar_bytes,
                              .align_offset = 0);
   nir_def *a = nir_load_ssbo(&b, 4, bit_size, nir_imm_int(&b, 1),
                              nir_imm_int(&b, 0), .align_mul = quat_bytes,
                              .align_offset = 0);
   nir_def *prod = nir_fmul(&b, a, s);
   nir_store_ssbo(&b, prod, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = quat_bytes,
                  .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_qfmul_form(void)
{
   return build_qfmul_variant(32, false, 0);
}

/* Single-input affine unary map: out[gid] = in[gid] * 2.0 + 1.0 (scalar float,
 * the 00_admissible_fma kernel shape -- one load, fmul by c0, fadd c1, store). */
static nir_shader *
build_unary_map_scalar(void)
{
   nir_builder b = cs_builder("cs_unary_map_scalar");
   nir_def *x = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_fadd(&b, nir_fmul(&b, x, nir_imm_float(&b, 2.0f)),
                         nir_imm_float(&b, 1.0f));
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Single-input transcendental map: out[gid] = f(in[gid]) for one transcendental
 * nir_op (one load, the op, store). */
static nir_shader *
build_unary_transcendental(nir_op op)
{
   nir_builder b = cs_builder("cs_unary_transcendental");
   nir_def *x = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_build_alu1(&b, op, x);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Reciprocal as GLSL spells it: out[gid] = numer / in[gid], reaching the
 * detector as fdiv(numer, x) (the classify clone does not lower fdiv).  A unit
 * numerator is the reciprocal arm (recorded as frcp); a non-unit numerator is a
 * scaled reciprocal the detector must reject. */
static nir_shader *
build_unary_reciprocal(float numer)
{
   nir_builder b = cs_builder("cs_unary_reciprocal");
   nir_def *x = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *y = nir_fdiv(&b, nir_imm_float(&b, numer), x);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Push-derived affine unary map: out[gid] = in[gid] * pc.c0 + pc.c1, the
 * post-explicit_io shape of a kernel reading its scale/bias from a
 * push_constant block.  c0 reads at byte offset c0_off, c1 at c1_off; a
 * dynamic_offset variant feeds a non-constant offset to the c1 load so the
 * detector's compile-time-offset requirement is exercised. */
static nir_shader *
build_unary_map_scalar_push(uint32_t c0_off, uint32_t c1_off,
                            bool dynamic_offset, bool literal_c0)
{
   nir_builder b = cs_builder("cs_unary_map_scalar_push");
   nir_def *x = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *c0 = literal_c0 ?
      nir_imm_float(&b, 2.0f) :
      nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, c0_off),
                             .base = 0, .range = 128);
   /* The dynamic-offset variant keys the c1 read on the invocation id (NOT a
    * second load_ssbo, which would trip the one-load structural guard before
    * the offset check ever ran). */
   nir_def *c1_offset = dynamic_offset ?
      nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0) :
      nir_imm_int(&b, c1_off);
   nir_def *c1 = nir_load_push_constant(&b, 1, 32, c1_offset,
                                        .base = 0, .range = 128);
   nir_def *y = nir_fadd(&b, nir_fmul(&b, x, c0), c1);
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Vec4 push-derived unary map: the scalar push constants broadcast across the
 * vec4 lanes, the splat the nir builder expresses as an all-lanes-equal
 * swizzle of the scalar load_push_constant def. */
static nir_shader *
build_unary_map_vec4_push(void)
{
   nir_builder b = cs_builder("cs_unary_map_vec4_push");
   nir_def *x = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *c0 = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                        .base = 0, .range = 128);
   nir_def *c1 = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 4),
                                        .base = 0, .range = 128);
   const unsigned splat[4] = { 0, 0, 0, 0 };
   nir_def *y = nir_fadd(&b, nir_fmul(&b, x, nir_swizzle(&b, c0, splat, 4)),
                         nir_swizzle(&b, c1, splat, 4));
   nir_store_ssbo(&b, y, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

/* Box-3 multi-tap gather, direct-offset shape: every tap offset is the store
 * offset def itself or iadd(store_offset, +/-4) -- the pre-scaled form a
 * synthetic kernel builds without descriptor-base arithmetic. */
static nir_shader *
build_multitap_box3_direct(void)
{
   nir_builder b = cs_builder("cs_multitap_box3_direct");
   nir_def *gid = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_def *bind = nir_imm_int(&b, 0);
   nir_def *t_l = nir_load_ssbo(&b, 1, 32, bind, nir_iadd_imm(&b, off, -4),
                                .align_mul = 4, .align_offset = 0);
   nir_def *t_c = nir_load_ssbo(&b, 1, 32, bind, off,
                                .align_mul = 4, .align_offset = 0);
   nir_def *t_r = nir_load_ssbo(&b, 1, 32, bind, nir_iadd_imm(&b, off, 4),
                                .align_mul = 4, .align_offset = 0);
   nir_def *sum = nir_iadd(&b, nir_iadd(&b, t_l, t_c), t_r);
   nir_store_ssbo(&b, sum, nir_imm_int(&b, 1), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Box-3 multi-tap gather, descriptor-base shape: the post-explicit_io SPIR-V
 * form where every ssbo byte offset is iadd(descriptor_base, index*4), the
 * input and output bases are DIFFERENT opaque defs, and the edge taps scale
 * iadd(gid, +/-1).  The opaque bases here are push-constant loads standing in
 * for the descriptor-chain defs real lowering produces.  bad_delta moves the
 * right tap to gid+2 so the exact-box-3 check is exercised.  The left tap is
 * isub(gid, 1), the exact opcode SPIR-V OpISub reaches the detector as (no
 * algebraic canonicalization runs in the classify prep). */
static nir_shader *
build_multitap_box3_desc_base(bool bad_delta)
{
   nir_builder b = cs_builder("cs_multitap_box3_desc_base");
   nir_def *gid = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *in_base = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                             .base = 0, .range = 128);
   nir_def *out_base = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 4),
                                              .base = 0, .range = 128);
   nir_def *stride = nir_imm_int(&b, 4);
   nir_def *scaled = nir_imul(&b, gid, stride);
   nir_def *off_l = nir_iadd(&b, in_base,
                             nir_imul(&b, nir_isub(&b, gid, nir_imm_int(&b, 1)),
                                      stride));
   nir_def *off_c = nir_iadd(&b, in_base, scaled);
   nir_def *off_r = nir_iadd(&b, in_base,
                             nir_imul(&b, nir_iadd_imm(&b, gid,
                                                       bad_delta ? 2 : 1),
                                      stride));
   nir_def *bind = nir_imm_int(&b, 0);
   nir_def *t_l = nir_load_ssbo(&b, 1, 32, bind, off_l,
                                .align_mul = 4, .align_offset = 0);
   nir_def *t_c = nir_load_ssbo(&b, 1, 32, bind, off_c,
                                .align_mul = 4, .align_offset = 0);
   nir_def *t_r = nir_load_ssbo(&b, 1, 32, bind, off_r,
                                .align_mul = 4, .align_offset = 0);
   nir_def *sum = nir_iadd(&b, nir_iadd(&b, t_l, t_c), t_r);
   nir_store_ssbo(&b, sum, nir_imm_int(&b, 1), nir_iadd(&b, out_base, scaled),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_unary_map_vec4(bool non_uniform_const, bool swizzled_input,
                     uint32_t output_binding)
{
   nir_builder b = cs_builder("cs_unary_map_vec4");
   nir_def *x = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   if (swizzled_input) {
      const unsigned swiz[4] = { 1, 0, 2, 3 };
      x = nir_swizzle(&b, x, swiz, 4);
   }
   nir_def *scale = non_uniform_const ?
      nir_imm_vec4(&b, 1.0f, 2.0f, 3.0f, 4.0f) :
      nir_imm_vec4(&b, 2.0f, 2.0f, 2.0f, 2.0f);
   nir_def *bias = nir_imm_vec4(&b, 1.0f, 1.0f, 1.0f, 1.0f);
   nir_def *y = nir_fadd(&b, nir_fmul(&b, x, scale), bias);
   nir_store_ssbo(&b, y, nir_imm_int(&b, output_binding), nir_imm_int(&b, 0),
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

/* Quaternion conjugate (a.x, -a.y, -a.z, -a.w) of a single input quaternion.
 * bad_sign leaves a.y un-negated so the negative case exercises the detector's
 * exact-sign check (the scalar lane stays positive, the three vector lanes
 * negate). */
static nir_shader *
build_qconj_form(bool bad_sign)
{
   nir_builder b = cs_builder("cs_qconj_f32vec4");
   nir_def *a = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *ax = nir_channel(&b, a, 0), *ay = nir_channel(&b, a, 1),
           *az = nir_channel(&b, a, 2), *aw = nir_channel(&b, a, 3);
   nir_def *conj = nir_vec4(&b, ax, bad_sign ? ay : nir_fneg(&b, ay),
                            nir_fneg(&b, az), nir_fneg(&b, aw));
   nir_store_ssbo(&b, conj, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

/* Quaternion squared norm dot(a, a) broadcast to four lanes -- the QNORM splat
 * the substrate's vec4 FP16 readback carries. */
static nir_shader *
build_qnorm_form(void)
{
   nir_builder b = cs_builder("cs_qnorm_f32vec4");
   nir_def *a = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *n = nir_fdot(&b, a, a);
   nir_def *bn = nir_vec4(&b, n, n, n, n);
   nir_store_ssbo(&b, bn, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static nir_def *
dp4_index_offset(nir_builder *b, unsigned element_bytes, unsigned stride_scale,
                 unsigned element_bias)
{
   nir_def *gid = nir_load_global_invocation_index(b, 32);
   nir_def *off =
      nir_imul(b, gid, nir_imm_int(b, (int)(element_bytes * stride_scale)));
   if (element_bias)
      off = nir_iadd_imm(b, off, (int)(element_bytes * element_bias));
   return off;
}

static nir_op
dp4_dot_op(unsigned components, bool replicated)
{
   switch (components) {
   case 2:
      return replicated ? nir_op_fdot2_replicated : nir_op_fdot2;
   case 3:
      return replicated ? nir_op_fdot3_replicated : nir_op_fdot3;
   default:
      return replicated ? nir_op_fdot4_replicated : nir_op_fdot4;
   }
}

static nir_def *
dp4_dot(nir_builder *b, unsigned components, bool replicated,
        nir_def *a, nir_def *c)
{
   nir_op op = dp4_dot_op(components, replicated);

   if (!replicated)
      return nir_build_alu2(b, op, a, c);

   nir_alu_instr *dot = nir_alu_instr_create(b->shader, op);
   nir_def_init(&dot->instr, &dot->def, components, 32);
   dot->fp_math_ctrl = nir_op_valid_fp_math_ctrl(dot->op, b->fp_math_ctrl);
   dot->src[0].src = nir_src_for_ssa(a);
   dot->src[1].src = nir_src_for_ssa(c);
   nir_builder_instr_insert(b, &dot->instr);
   return &dot->def;
}

/* Scalar DP4 dispatch shape: out_uint[gid] = f2u32(dot(a[gid], b[gid])).
 * fdot2 input records are 8 bytes.  fdot3 and fdot4 use the 16-byte FP32x4
 * replay carrier because R300 has no R32G32B32_FLOAT sampler target.  The
 * replicated opcode form models r300's fdot_replicates NIR option. */
static nir_shader *
build_dp4_u32(unsigned components, unsigned input_b_stride_scale,
              unsigned input_b_element_bias, unsigned output_stride_scale,
              bool cast_to_uint, bool replicated_dot, unsigned write_mask)
{
   nir_builder b = cs_builder("cs_dp4_u32");
   const unsigned input_bytes = components == 2 ? 8 : 16;
   nir_def *a = nir_load_ssbo(&b, components, 32, nir_imm_int(&b, 0),
                              dp4_index_offset(&b, input_bytes, 1, 0),
                              .align_mul = input_bytes, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, components, 32, nir_imm_int(&b, 1),
                              dp4_index_offset(&b, input_bytes,
                                               input_b_stride_scale,
                                               input_b_element_bias),
                              .align_mul = input_bytes, .align_offset = 0);
   nir_def *dot = dp4_dot(&b, components, replicated_dot, a, c);
   nir_store_ssbo(&b, cast_to_uint ? nir_f2u32(&b, dot) : dot,
                  nir_imm_int(&b, 2),
                  dp4_index_offset(&b, 4, output_stride_scale, 0),
                  .write_mask = write_mask, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_dp4_metadata(void)
{
   printf("dp4 detector\n");

   nir_shader *dp4 = build_dp4_u32(4, 1, 0, 1, true, false, 0x1);
   struct r300_compute_dp4_pattern p = {0};
   prepare_detect_shader(dp4);
   r300_nir_detect_dp4_pattern(dp4, &p);
   CHECK(p.is_dp4, "fdot4 f2u32 shape detects DP4");
   CHECK(p.components == 4 && p.dot_op == nir_op_fdot4,
         "fdot4 metadata records dot width and op");
   CHECK(p.input_a_ssbo_binding == 0 && p.input_b_ssbo_binding == 1 &&
         p.output_ssbo_binding == 2,
         "fdot4 metadata records three bindings");
   ralloc_free(dp4);

   nir_shader *dp2 = build_dp4_u32(2, 1, 0, 1, true, false, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(dp2);
   r300_nir_detect_dp4_pattern(dp2, &p);
   CHECK(p.is_dp4 && p.components == 2 && p.dot_op == nir_op_fdot2,
         "fdot2 shape detects with 8-byte input stride");
   ralloc_free(dp2);

   nir_shader *dp3 = build_dp4_u32(3, 1, 0, 1, true, false, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(dp3);
   r300_nir_detect_dp4_pattern(dp3, &p);
   CHECK(p.is_dp4 && p.components == 3 && p.dot_op == nir_op_fdot3,
         "fdot3 shape detects with 16-byte input carrier");
   ralloc_free(dp3);

   nir_shader *rep4 = build_dp4_u32(4, 1, 0, 1, true, true, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(rep4);
   r300_nir_detect_dp4_pattern(rep4, &p);
   CHECK(p.is_dp4 && p.components == 4 && p.dot_op == nir_op_fdot4_replicated,
         "fdot4_replicated shape detects DP4");
   ralloc_free(rep4);

   nir_shader *rep2 = build_dp4_u32(2, 1, 0, 1, true, true, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(rep2);
   r300_nir_detect_dp4_pattern(rep2, &p);
   CHECK(p.is_dp4 && p.components == 2 && p.dot_op == nir_op_fdot2_replicated,
         "fdot2_replicated shape detects DP4");
   ralloc_free(rep2);

   nir_shader *rep3 = build_dp4_u32(3, 1, 0, 1, true, true, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(rep3);
   r300_nir_detect_dp4_pattern(rep3, &p);
   CHECK(p.is_dp4 && p.components == 3 && p.dot_op == nir_op_fdot3_replicated,
         "fdot3_replicated shape detects DP4");
   ralloc_free(rep3);

   nir_shader *masked = build_dp4_u32(4, 1, 0, 1, true, true, 0x7);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(masked);
   r300_nir_detect_dp4_pattern(masked, &p);
   CHECK(!p.is_dp4, "partial write mask rejects DP4 replay");
   ralloc_free(masked);

   nir_shader *shifted = build_dp4_u32(4, 1, 1, 1, true, false, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(shifted);
   r300_nir_detect_dp4_pattern(shifted, &p);
   CHECK(!p.is_dp4, "shifted input offset rejects DP4 replay");
   ralloc_free(shifted);

   nir_shader *strided = build_dp4_u32(4, 1, 0, 2, true, false, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(strided);
   r300_nir_detect_dp4_pattern(strided, &p);
   CHECK(!p.is_dp4, "strided output offset rejects DP4 replay");
   ralloc_free(strided);

   nir_shader *plain_float = build_dp4_u32(4, 1, 0, 1, false, false, 0x1);
   memset(&p, 0, sizeof(p));
   prepare_detect_shader(plain_float);
   r300_nir_detect_dp4_pattern(plain_float, &p);
   CHECK(!p.is_dp4, "plain float dot rejects DP4 integer-encode replay");
   ralloc_free(plain_float);
}

/* The four Hamilton second-operand permutations of q (w,x,y,z layout), the
 * vec4s the canonical 4-dot product dots the first operand against. */
static void
ham_perms(nir_builder *b, nir_def *q, nir_def *out[4])
{
   nir_def *x = nir_channel(b, q, 0), *y = nir_channel(b, q, 1);
   nir_def *z = nir_channel(b, q, 2), *w = nir_channel(b, q, 3);
   nir_def *ny = nir_fneg(b, y), *nz = nir_fneg(b, z), *nw = nir_fneg(b, w);
   out[0] = nir_vec4(b, x, ny, nz, nw);
   out[1] = nir_vec4(b, y, x, w, nz);
   out[2] = nir_vec4(b, z, nw, x, y);
   out[3] = nir_vec4(b, w, z, ny, x);
}
static nir_def *
ham_prod(nir_builder *b, nir_def *p, nir_def *q)
{
   nir_def *pm[4];
   ham_perms(b, q, pm);
   return nir_vec4(b, nir_fdot(b, p, pm[0]), nir_fdot(b, p, pm[1]),
                   nir_fdot(b, p, pm[2]), nir_fdot(b, p, pm[3]));
}
static nir_def *
qconj4(nir_builder *b, nir_def *q)
{
   return nir_vec4(b, nir_channel(b, q, 0), nir_fneg(b, nir_channel(b, q, 1)),
                   nir_fneg(b, nir_channel(b, q, 2)), nir_fneg(b, nir_channel(b, q, 3)));
}
/* The four conj-composed (rotation outer) permutations of q -- the Hamilton rows
 * folded with a conjugate over the SAME load, the form the compiler collapses
 * b*conj(q) into (the perm channels stay references to q, not to a conj(q)
 * intermediate vec4 that CSE could alias with another product's row). */
static void
qrot_perms(nir_builder *b, nir_def *q, nir_def *out[4])
{
   nir_def *x = nir_channel(b, q, 0), *y = nir_channel(b, q, 1);
   nir_def *z = nir_channel(b, q, 2), *w = nir_channel(b, q, 3);
   nir_def *ny = nir_fneg(b, y), *nz = nir_fneg(b, z), *nw = nir_fneg(b, w);
   out[0] = nir_vec4(b, x, y, z, w);
   out[1] = nir_vec4(b, ny, x, nw, z);
   out[2] = nir_vec4(b, nz, w, x, ny);
   out[3] = nir_vec4(b, nw, nz, y, x);
}
static nir_def *
qrot_prod(nir_builder *b, nir_def *p, nir_def *q)
{
   nir_def *pm[4];
   qrot_perms(b, q, pm);
   return nir_vec4(b, nir_fdot(b, p, pm[0]), nir_fdot(b, p, pm[1]),
                   nir_fdot(b, p, pm[2]), nir_fdot(b, p, pm[3]));
}

/* Octonion product (a,b)*(c,d) = (a*c - conj(d)*b, d*a + b*conj(c)) split into
 * four quaternion input loads (a,b,c,d in order) and two output stores (o_lo,
 * o_hi) -- the eight-wide form the OMUL detector admits.  Sixteen DP4s total. */
static nir_shader *
build_omul_form(void)
{
   nir_builder b = cs_builder("cs_omul_f32vec4");
   nir_def *a = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *bb = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *d = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 3), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *olo = nir_fsub(&b, ham_prod(&b, a, c), ham_prod(&b, qconj4(&b, d), bb));
   nir_def *ohi = nir_fadd(&b, ham_prod(&b, d, a), qrot_prod(&b, bb, c));
   nir_store_ssbo(&b, olo, nir_imm_int(&b, 4), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   nir_store_ssbo(&b, ohi, nir_imm_int(&b, 5), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static nir_def *
ld(nir_builder *b, unsigned binding)
{
   return nir_load_ssbo(b, 4, 32, nir_imm_int(b, binding), nir_imm_int(b, 0),
                        .align_mul = 16, .align_offset = 0);
}
static void
st(nir_builder *b, nir_def *v, unsigned binding)
{
   nir_store_ssbo(b, v, nir_imm_int(b, binding), nir_imm_int(b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
}

/* Octonion add/sub: o_lo = a (+|-) c, o_hi = b (+|-) d.  Loads a,b,c,d in order. */
static nir_shader *
build_oaddsub_form(bool is_sub)
{
   nir_builder b = cs_builder("cs_oaddsub_f32vec4");
   nir_def *a = ld(&b, 0), *bb = ld(&b, 1), *c = ld(&b, 2), *d = ld(&b, 3);
   st(&b, is_sub ? nir_fsub(&b, a, c) : nir_fadd(&b, a, c), 4);
   st(&b, is_sub ? nir_fsub(&b, bb, d) : nir_fadd(&b, bb, d), 5);
   return b.shader;
}

/* Octonion conjugate: o_lo = (a.x,-a.y,-a.z,-a.w), o_hi = -b. */
static nir_shader *
build_oconj_form(void)
{
   nir_builder b = cs_builder("cs_oconj_f32vec4");
   nir_def *a = ld(&b, 0), *bb = ld(&b, 1);
   st(&b, nir_vec4(&b, nir_channel(&b, a, 0), nir_fneg(&b, nir_channel(&b, a, 1)),
                   nir_fneg(&b, nir_channel(&b, a, 2)), nir_fneg(&b, nir_channel(&b, a, 3))), 2);
   st(&b, nir_fneg(&b, bb), 3);
   return b.shader;
}

/* Octonion squared norm: out = vec4(dot(a,a) + dot(b,b)). */
static nir_shader *
build_onorm_form(void)
{
   nir_builder b = cs_builder("cs_onorm_f32vec4");
   nir_def *a = ld(&b, 0), *bb = ld(&b, 1);
   nir_def *n = nir_fadd(&b, nir_fdot(&b, a, a), nir_fdot(&b, bb, bb));
   st(&b, nir_vec4(&b, n, n, n, n), 2);
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
      /* Parity with the r3v classify prep: fold offset arithmetic to the
       * inline constants the detectors capture from. */
      NIR_PASS(progress, nir, nir_opt_constant_folding);
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

/* Verify that r300_nir_detect_binary_map normalises binding capture to
 * operand order rather than load-emission order when the ba case fires.
 * build_binary_map_isub_ba emits load(0) then load(1) but the op is
 * isub(load1, load0): the left operand (src[0]) is load1=binding 1 and the
 * right operand (src[1]) is load0=binding 0.  input_a must capture binding 1
 * and input_b must capture binding 0 so the synthesised FS computes
 * binding_1 - binding_0, which matches the kernel's semantics. */
static void
case_binary_map_isub_ba_operand_order(void)
{
   nir_shader *nir = build_binary_map_isub_ba();
   struct r300_compute_binary_map_pattern binmap = {0};

   prepare_detect_shader(nir);
   r300_nir_detect_binary_map(nir, &binmap);
   CHECK(binmap.is_binary_map, "isub ba: shape detected");
   CHECK(binmap.alu_op == nir_op_isub, "isub ba: opcode is isub");
   CHECK(binmap.input_a_ssbo_binding == 1,
         "isub ba: input_a captures left-operand binding (1)");
   CHECK(binmap.input_b_ssbo_binding == 0,
         "isub ba: input_b captures right-operand binding (0)");
   ralloc_free(nir);
}

/* Coverage for the offset_scalar_semantically_equal gate in
 * r300_nir_detect_identity_map / r300_nir_detect_binary_map: separately-
 * lowered-but-pointwise-equal address chains must still admit, and a genuine
 * scatter (index reversal or transposed vector component) must reject.  None
 * of these call prepare_detect_shader first -- the point is to exercise the
 * walker's own structural matching directly, not rely on nir_opt_cse having
 * already merged the two chains into one shared def. */
static void
case_identity_binary_map_offset_gate(void)
{
   printf("identity/binary-map offset-equivalence gate\n");

   {
      nir_shader *nir = build_identity_map_gid_separate_offset_chains();
      struct r300_compute_identity_pattern ident = {0};
      r300_nir_detect_identity_map(nir, &ident);
      CHECK(ident.is_identity_map,
            "identity-map: separately-lowered matching gid*4 offsets admit");
      ralloc_free(nir);
   }
   {
      nir_shader *nir = build_identity_map_scatter_reversed();
      struct r300_compute_identity_pattern ident = {0};
      r300_nir_detect_identity_map(nir, &ident);
      CHECK(!ident.is_identity_map,
            "identity-map: reversed-index scatter rejects");
      ralloc_free(nir);
   }
   {
      nir_shader *nir = build_identity_map_scatter_transposed_component();
      struct r300_compute_identity_pattern ident = {0};
      r300_nir_detect_identity_map(nir, &ident);
      CHECK(!ident.is_identity_map,
            "identity-map: transposed .x/.y component scatter rejects");
      ralloc_free(nir);
   }
   {
      nir_shader *nir = build_binary_map_gid_separate_offset_chains();
      struct r300_compute_binary_map_pattern binmap = {0};
      r300_nir_detect_binary_map(nir, &binmap);
      CHECK(binmap.is_binary_map,
            "binary-map: separately-lowered matching gid*4 offsets admit");
      ralloc_free(nir);
   }
   {
      nir_shader *nir = build_binary_map_scatter_reversed();
      struct r300_compute_binary_map_pattern binmap = {0};
      r300_nir_detect_binary_map(nir, &binmap);
      CHECK(!binmap.is_binary_map,
            "binary-map: reversed-index scatter rejects");
      ralloc_free(nir);
   }
}

static void
case_qfmul_metadata(void)
{
   nir_shader *nir = build_qfmul_form();
   struct r300_compute_admission adm;
   struct r300_compute_qfmul_pattern qf = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 qfmul kernel admits");
   r300_nir_detect_qfmul_pattern(nir, &qf);
   CHECK(qf.is_qfmul, "qfmul scalar-broadcast shape detected");
   CHECK(qf.scalar_ssbo_binding == 0, "qfmul metadata records scalar binding 0");
   CHECK(qf.quat_ssbo_binding == 1, "qfmul metadata records quaternion binding 1");
   CHECK(qf.output_ssbo_binding == 2, "qfmul metadata records output binding 2");
   CHECK(qf.scalar_ssbo_binding_valid && qf.quat_ssbo_binding_valid &&
         qf.output_ssbo_binding_valid,
         "qfmul metadata records explicit binding-zero roles");

   /* The 4-vs-1 width asymmetry must steer the kernel away from the binary-map
    * carrier: its orchestrator samples both inputs per-element, so reading the
    * one-float scalar buffer across the whole raster extent would run off its
    * end.  The binary-map width-equality guard declines it, handing it here. */
   struct r300_compute_binary_map_pattern binmap = {0};
   r300_nir_detect_binary_map(nir, &binmap);
   CHECK(!binmap.is_binary_map, "qfmul broadcast is not an equal-width binary map");
   ralloc_free(nir);

   /* The converse: an equal-width vec4+vec4 binary map is a genuine elementwise
    * carrier, not a scalar broadcast; the qfmul detector's 1-component splat
    * requirement must reject it so the two classes stay disjoint. */
   nir_shader *bin = build_binary_map_f32vec4();
   struct r300_compute_qfmul_pattern bin_qf = {0};
   prepare_detect_shader(bin);
   r300_nir_detect_qfmul_pattern(bin, &bin_qf);
   CHECK(!bin_qf.is_qfmul, "qfmul rejects an equal-width binary map");
   ralloc_free(bin);

   nir_shader *scalar_delta = build_qfmul_variant(32, false, 4);
   struct r300_compute_qfmul_pattern delta_qf = {0};
   prepare_detect_shader(scalar_delta);
   r300_nir_detect_qfmul_pattern(scalar_delta, &delta_qf);
   CHECK(!delta_qf.is_qfmul, "qfmul rejects a nonzero scalar byte offset");
   ralloc_free(scalar_delta);

   nir_shader *per_element_scalar = build_qfmul_variant(32, true, 0);
   struct r300_compute_qfmul_pattern per_element_qf = {0};
   prepare_detect_shader(per_element_scalar);
   r300_nir_detect_qfmul_pattern(per_element_scalar, &per_element_qf);
   CHECK(!per_element_qf.is_qfmul,
         "qfmul rejects a per-invocation scalar offset");
   ralloc_free(per_element_scalar);

   nir_shader *fp16 = build_qfmul_variant(16, false, 0);
   struct r300_compute_qfmul_pattern fp16_qf = {0};
   prepare_detect_shader(fp16);
   r300_nir_detect_qfmul_pattern(fp16, &fp16_qf);
   CHECK(!fp16_qf.is_qfmul, "qfmul rejects non-32-bit operands");
   ralloc_free(fp16);
}

static void
case_unary_metadata(void)
{
   nir_shader *nir = build_unary_map_scalar();
   struct r300_compute_admission adm;
   struct r300_compute_unary_map_pattern umap = {0};
   struct r300_compute_binary_map_pattern binmap = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "scalar unary affine-map kernel admits");
   r300_nir_detect_unary_map(nir, &umap);
   CHECK(umap.is_unary_map,
         "scalar unary-map detected via the R32_FLOAT scalar carrier");
   CHECK(umap.value_components == 1, "scalar unary-map records scalar width");
   CHECK(umap.value_bit_size == 32, "scalar unary-map records 32-bit lane");
   CHECK(umap.mul_const == 2.0f, "scalar unary-map records c0 scale 2.0");
   CHECK(umap.add_const == 1.0f, "scalar unary-map records c1 bias 1.0");
   CHECK(umap.input_ssbo_binding_valid &&
         umap.input_ssbo_binding == 0,
         "scalar unary-map records input binding 0");
   CHECK(umap.output_ssbo_binding_valid &&
         umap.output_ssbo_binding == 1,
         "scalar unary-map records output binding 1");
   /* One load means it is not the two-input binary-map shape. */
   r300_nir_detect_binary_map(nir, &binmap);
   CHECK(!binmap.is_binary_map, "unary-map shape is not a binary map");
   ralloc_free(nir);

   nir_shader *vec_uniform = build_unary_map_vec4(false, false, 1);
   struct r300_compute_unary_map_pattern vec_map = {0};
   prepare_detect_shader(vec_uniform);
   r300_nir_detect_unary_map(vec_uniform, &vec_map);
   CHECK(vec_map.is_unary_map, "vec4 unary-map accepts uniform constants");
   CHECK(vec_map.mul_const == 2.0f, "vec4 unary-map records c0 scale 2.0");
   CHECK(vec_map.add_const == 1.0f, "vec4 unary-map records c1 bias 1.0");
   CHECK(vec_map.value_components == 4, "vec4 unary-map records vector width");
   CHECK(vec_map.value_bit_size == 32, "vec4 unary-map records 32-bit lane");
   CHECK(vec_map.value_is_float, "vec4 unary-map records float result");
   CHECK(vec_map.input_ssbo_binding_valid,
         "vec4 unary-map records real input binding 0");
   CHECK(vec_map.output_ssbo_binding_valid,
         "vec4 unary-map records real output binding 1");
   ralloc_free(vec_uniform);

   nir_shader *inplace = build_unary_map_vec4(false, false, 0);
   struct r300_compute_unary_map_pattern same_binding = {0};
   prepare_detect_shader(inplace);
   r300_nir_detect_unary_map(inplace, &same_binding);
   CHECK(same_binding.is_unary_map, "in-place unary-map shape detected");
   CHECK(same_binding.input_ssbo_binding_valid &&
         same_binding.output_ssbo_binding_valid,
         "in-place unary-map preserves explicit zero bindings");
   CHECK(same_binding.input_ssbo_binding == 0 &&
         same_binding.output_ssbo_binding == 0,
         "in-place unary-map records binding zero for input and output");
   ralloc_free(inplace);

   nir_shader *vec_non_uniform = build_unary_map_vec4(true, false, 1);
   struct r300_compute_unary_map_pattern non_uniform = {0};
   prepare_detect_shader(vec_non_uniform);
   r300_nir_detect_unary_map(vec_non_uniform, &non_uniform);
   CHECK(!non_uniform.is_unary_map,
         "vec4 unary-map rejects non-uniform constants");
   ralloc_free(vec_non_uniform);

   nir_shader *vec_swizzled = build_unary_map_vec4(false, true, 1);
   struct r300_compute_unary_map_pattern swizzled = {0};
   prepare_detect_shader(vec_swizzled);
   r300_nir_detect_unary_map(vec_swizzled, &swizzled);
   CHECK(!swizzled.is_unary_map, "vec4 unary-map rejects swizzled inputs");
   ralloc_free(vec_swizzled);

   /* A genuine two-input binary map must NOT match the unary detector. */
   nir_shader *bin = build_binary_map_f32vec4();
   struct r300_compute_unary_map_pattern not_unary = {0};
   prepare_detect_shader(bin);
   r300_nir_detect_unary_map(bin, &not_unary);
   CHECK(!not_unary.is_unary_map, "two-input binary map rejected by unary detector");
   ralloc_free(bin);
}

static void
case_unary_push_metadata(void)
{
   /* Both constants push-derived at offsets 0 / 4. */
   nir_shader *nir = build_unary_map_scalar_push(0, 4, false, false);
   struct r300_compute_admission adm;
   struct r300_compute_unary_map_pattern umap = {0};
   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "push-constant unary affine-map kernel admits");
   r300_nir_detect_unary_map(nir, &umap);
   CHECK(umap.is_unary_map, "push-derived scalar unary-map detected");
   CHECK(umap.mul_const_from_push, "c0 recorded as push-derived");
   CHECK(umap.add_const_from_push, "c1 recorded as push-derived");
   CHECK(umap.mul_const_push_offset == 0, "c0 push offset 0 recorded");
   CHECK(umap.add_const_push_offset == 4, "c1 push offset 4 recorded");
   CHECK(umap.value_components == 1, "push-derived map records scalar width");
   ralloc_free(nir);

   /* Mixed: literal c0, push-derived c1 at a non-zero offset. */
   nir_shader *mixed = build_unary_map_scalar_push(0, 12, false, true);
   struct r300_compute_unary_map_pattern mixed_map = {0};
   prepare_detect_shader(mixed);
   r300_nir_detect_unary_map(mixed, &mixed_map);
   CHECK(mixed_map.is_unary_map, "mixed literal/push unary-map detected");
   CHECK(!mixed_map.mul_const_from_push, "literal c0 stays literal");
   CHECK(mixed_map.mul_const == 2.0f, "literal c0 value 2.0 recorded");
   CHECK(mixed_map.add_const_from_push, "mixed c1 recorded as push-derived");
   CHECK(mixed_map.add_const_push_offset == 12, "c1 push offset 12 recorded");
   ralloc_free(mixed);

   /* A push read at a dynamic offset has no compile-time constant-file
    * address: the shape must not match. */
   nir_shader *dyn = build_unary_map_scalar_push(0, 4, true, false);
   struct r300_compute_unary_map_pattern dyn_map = {0};
   prepare_detect_shader(dyn);
   r300_nir_detect_unary_map(dyn, &dyn_map);
   CHECK(!dyn_map.is_unary_map,
         "dynamic push-constant offset rejected by unary detector");
   ralloc_free(dyn);

   /* An offset past the 128-byte push window has no FS CONST[0..7] slot. */
   nir_shader *oob = build_unary_map_scalar_push(0, 128, false, false);
   struct r300_compute_unary_map_pattern oob_map = {0};
   prepare_detect_shader(oob);
   r300_nir_detect_unary_map(oob, &oob_map);
   CHECK(!oob_map.is_unary_map,
         "push offset past the 128-byte window rejected");
   ralloc_free(oob);

   /* Vec4 splat of scalar push constants: all-lanes-equal swizzle matches. */
   nir_shader *vec = build_unary_map_vec4_push();
   struct r300_compute_unary_map_pattern vec_map = {0};
   prepare_detect_shader(vec);
   r300_nir_detect_unary_map(vec, &vec_map);
   CHECK(vec_map.is_unary_map, "vec4 splat push unary-map detected");
   CHECK(vec_map.mul_const_from_push && vec_map.add_const_from_push,
         "vec4 splat records both constants as push-derived");
   CHECK(vec_map.mul_const_push_offset == 0 &&
         vec_map.add_const_push_offset == 4,
         "vec4 splat records push offsets 0 and 4");
   CHECK(vec_map.value_components == 4, "vec4 splat records vector width");
   ralloc_free(vec);
}

static void
case_unary_transcendental_metadata(void)
{
   /* Each admitted transcendental op detects, records its op, admits, and is
    * NOT mistaken for the affine unary-map. */
   const nir_op ops[] = { nir_op_fsqrt, nir_op_frsq, nir_op_frcp, nir_op_fexp2,
                          nir_op_flog2, nir_op_fsin, nir_op_fcos, nir_op_ffract,
                          nir_op_ffloor, nir_op_fround_even };
   for (unsigned i = 0; i < ARRAY_SIZE(ops); i++) {
      nir_shader *nir = build_unary_transcendental(ops[i]);
      struct r300_compute_admission adm;
      struct r300_compute_unary_transcendental_pattern tr = {0};
      struct r300_compute_unary_map_pattern um = {0};
      prepare_detect_shader(nir);
      r300_nir_classify_compute(nir, &adm);
      CHECK(adm.admissible, "transcendental kernel admits");
      r300_nir_detect_unary_transcendental(nir, &tr);
      CHECK(tr.is_unary_transcendental, "transcendental op detected");
      CHECK(tr.alu_op == ops[i], "transcendental records its nir_op");
      CHECK(tr.value_components == 1 && tr.value_bit_size == 32,
            "transcendental records scalar 32-bit");
      CHECK(tr.input_ssbo_binding == 0 && tr.output_ssbo_binding == 1,
            "transcendental records bindings 0 -> 1");
      /* The affine unary-map detector must NOT fire on a transcendental. */
      r300_nir_detect_unary_map(nir, &um);
      CHECK(!um.is_unary_map, "transcendental is not an affine unary-map");
      ralloc_free(nir);
   }

   /* The reciprocal arm: GLSL 1.0/x reaches the detector as fdiv(1.0, x) and is
    * recorded as frcp; a non-unit numerator (2.0/x, a scaled reciprocal) must
    * NOT match. */
   nir_shader *rcp = build_unary_reciprocal(1.0f);
   struct r300_compute_unary_transcendental_pattern rcp_tr = {0};
   prepare_detect_shader(rcp);
   r300_nir_detect_unary_transcendental(rcp, &rcp_tr);
   CHECK(rcp_tr.is_unary_transcendental, "reciprocal 1.0/x admits");
   CHECK(rcp_tr.alu_op == nir_op_frcp, "reciprocal records frcp");
   CHECK(rcp_tr.input_ssbo_binding == 0 && rcp_tr.output_ssbo_binding == 1,
         "reciprocal records bindings 0 -> 1");
   ralloc_free(rcp);

   nir_shader *scaled = build_unary_reciprocal(2.0f);
   struct r300_compute_unary_transcendental_pattern scaled_tr = {0};
   prepare_detect_shader(scaled);
   r300_nir_detect_unary_transcendental(scaled, &scaled_tr);
   CHECK(!scaled_tr.is_unary_transcendental,
         "scaled reciprocal 2.0/x rejected (non-unit numerator)");
   ralloc_free(scaled);

   /* The affine map (fmul/fadd) must NOT be mistaken for a transcendental --
    * the two detectors are disjoint by op set. */
   nir_shader *aff = build_unary_map_scalar();
   struct r300_compute_unary_transcendental_pattern aff_tr = {0};
   prepare_detect_shader(aff);
   r300_nir_detect_unary_transcendental(aff, &aff_tr);
   CHECK(!aff_tr.is_unary_transcendental,
         "affine unary-map rejected by transcendental detector");
   ralloc_free(aff);
}

static void
case_binary_transcendental_metadata(void)
{
   /* fpow / fdiv at both carriers (scalar + vec4) detect, record their op,
    * width, and order-preserving bindings, admit, and are NOT mistaken for the
    * commutative binary_map. */
   const nir_op ops[] = { nir_op_fpow, nir_op_fdiv };
   const unsigned widths[] = { 1, 4 };
   for (unsigned w = 0; w < ARRAY_SIZE(widths); w++) {
      for (unsigned i = 0; i < ARRAY_SIZE(ops); i++) {
         nir_shader *nir = build_binary_transcendental(ops[i], false, widths[w]);
         struct r300_compute_admission adm;
         struct r300_compute_binary_transcendental_pattern bt = {0};
         struct r300_compute_binary_map_pattern bm = {0};
         prepare_detect_shader(nir);
         r300_nir_classify_compute(nir, &adm);
         CHECK(adm.admissible, "binary transcendental admits");
         r300_nir_detect_binary_transcendental(nir, &bt);
         CHECK(bt.is_binary_transcendental, "binary transcendental detected");
         CHECK(bt.alu_op == ops[i], "binary transcendental records its nir_op");
         CHECK(bt.input_a_ssbo_binding == 0 && bt.input_b_ssbo_binding == 1 &&
               bt.output_ssbo_binding == 2,
               "binary transcendental records bindings a=0 b=1 out=2");
         CHECK(bt.value_components == widths[w] && bt.value_bit_size == 32,
               "binary transcendental records its width (1 or 4) 32-bit");
         /* The commutative binary-map detector must NOT fire on fpow/fdiv. */
         r300_nir_detect_binary_map(nir, &bm);
         CHECK(!bm.is_binary_map, "binary transcendental is not a binary_map");
         ralloc_free(nir);
      }
   }

   /* Operand order is preserved: swapping the sources swaps input_a / input_b,
    * so a/b is never silently transposed to b/a. */
   nir_shader *swapped = build_binary_transcendental(nir_op_fdiv, true, 4);
   struct r300_compute_binary_transcendental_pattern sw = {0};
   prepare_detect_shader(swapped);
   r300_nir_detect_binary_transcendental(swapped, &sw);
   CHECK(sw.is_binary_transcendental && sw.input_a_ssbo_binding == 1 &&
         sw.input_b_ssbo_binding == 0,
         "swapped fdiv records input_a=1 input_b=0 (order preserved)");
   ralloc_free(swapped);

   /* The commutative binary map (fadd) must NOT be a binary transcendental. */
   nir_shader *add = build_binary_map_f32vec4();
   struct r300_compute_binary_transcendental_pattern add_bt = {0};
   prepare_detect_shader(add);
   r300_nir_detect_binary_transcendental(add, &add_bt);
   CHECK(!add_bt.is_binary_transcendental,
         "binary_map fadd rejected by binary-transcendental detector");
   ralloc_free(add);
}

static void
case_bitwise_logicop_metadata(void)
{
   /* iand / ior / ixor detect, record their op + bindings, admit, and are NOT
    * mistaken for binary_map (arithmetic) or binary_transcendental. */
   const nir_op ops[] = { nir_op_iand, nir_op_ior, nir_op_ixor };
   for (unsigned i = 0; i < ARRAY_SIZE(ops); i++) {
      nir_shader *nir = build_bitwise_logicop(ops[i]);
      struct r300_compute_admission adm;
      struct r300_compute_bitwise_logicop_pattern bw = {0};
      struct r300_compute_binary_map_pattern bm = {0};
      struct r300_compute_binary_transcendental_pattern bt = {0};
      prepare_detect_shader(nir);
      r300_nir_classify_compute(nir, &adm);
      CHECK(adm.admissible, "bitwise logicop admits");
      r300_nir_detect_bitwise_logicop(nir, &bw);
      CHECK(bw.is_bitwise_logicop, "bitwise logicop detected");
      CHECK(bw.alu_op == ops[i], "bitwise logicop records its nir_op");
      CHECK(bw.input_a_ssbo_binding == 0 && bw.input_b_ssbo_binding == 1 &&
            bw.output_ssbo_binding == 2,
            "bitwise logicop records bindings a=0 b=1 out=2");
      CHECK(bw.value_components == 1 && bw.value_bit_size == 32,
            "bitwise logicop records scalar 32-bit");
      r300_nir_detect_binary_map(nir, &bm);
      CHECK(!bm.is_binary_map, "bitwise logicop is not a binary_map");
      r300_nir_detect_binary_transcendental(nir, &bt);
      CHECK(!bt.is_binary_transcendental,
            "bitwise logicop is not a binary_transcendental");
      ralloc_free(nir);
   }

   /* The arithmetic binary map (fadd) must NOT match the bitwise detector --
    * the op sets are disjoint. */
   nir_shader *add = build_binary_map_f32vec4();
   struct r300_compute_bitwise_logicop_pattern add_bw = {0};
   prepare_detect_shader(add);
   r300_nir_detect_bitwise_logicop(add, &add_bw);
   CHECK(!add_bw.is_bitwise_logicop,
         "binary_map fadd rejected by bitwise-logicop detector");
   ralloc_free(add);
}

static void
case_shift_logical_metadata(void)
{
   /* ishl / ushr / ishr by a constant k in [1,31] admit and record their
    * direction, signedness, amount, and bindings.  The unary-transcendental
    * detector consumes the same one-load/one-store shape, so a single-input
    * integer shift is the case it could read as a single-input float op; the
    * test confirms it stays silent on every variant here. */
   const struct { nir_op op; bool is_left; bool is_arith; } variants[] = {
      { nir_op_ishl, true,  false }, { nir_op_ushr, false, false },
      { nir_op_ishr, false, true } };
   const uint32_t amounts[] = { 1, 8, 31 };
   for (unsigned v = 0; v < ARRAY_SIZE(variants); v++) {
      for (unsigned a = 0; a < ARRAY_SIZE(amounts); a++) {
         nir_shader *nir = build_shift_logical(variants[v].op, amounts[a]);
         struct r300_compute_admission adm;
         struct r300_compute_shift_logical_pattern sh = {0};
         struct r300_compute_unary_transcendental_pattern tr = {0};
         prepare_detect_shader(nir);
         r300_nir_classify_compute(nir, &adm);
         CHECK(adm.admissible, "shift admits");
         r300_nir_detect_shift_logical(nir, &sh);
         CHECK(sh.is_shift_logical, "shift detected");
         CHECK(sh.is_left == variants[v].is_left, "shift records direction");
         CHECK(sh.is_arithmetic == variants[v].is_arith,
               "shift records signedness");
         CHECK(sh.shift_amount == amounts[a], "shift records amount");
         CHECK(sh.input_ssbo_binding == 0 && sh.output_ssbo_binding == 1,
               "shift records bindings 0 -> 1");
         CHECK(sh.value_components == 1 && sh.value_bit_size == 32,
               "shift records scalar 32-bit");
         /* A unary integer shift must not look like the float transcendental. */
         r300_nir_detect_unary_transcendental(nir, &tr);
         CHECK(!tr.is_unary_transcendental, "shift is not transcendental");
         ralloc_free(nir);
      }
   }

   /* Constants the constant-shift detector MUST leave unmatched (so they no-op as
    * UNKNOWN_SHAPE rather than produce a wrong result): k = 0 (identity) and
    * k = 32 (GLSL-undefined).  The variable-amount shape is covered below. */
   const struct { nir_op op; uint32_t k; const char *msg; } negs[] = {
      { nir_op_ishl, 0,  "k=0 stays unmatched (identity)" },
      { nir_op_ishl, 32, "k=32 stays unmatched (undefined)" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(negs); i++) {
      nir_shader *nir = build_shift_logical(negs[i].op, negs[i].k);
      struct r300_compute_shift_logical_pattern sh = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_shift_logical(nir, &sh);
      CHECK(!sh.is_shift_logical, negs[i].msg);
      ralloc_free(nir);
   }
}

static void
case_shift_variable_metadata(void)
{
   /* out[gid] = a[gid] << b[gid] / >> b[gid] (unsigned or signed): the variable
    * detector claims it and records the value/amount/output bindings and the
    * signedness; the constant-shift detector (one load) leaves it alone. */
   const struct { nir_op op; bool is_left; bool is_arith; } variants[] = {
      { nir_op_ishl, true,  false },
      { nir_op_ushr, false, false },
      { nir_op_ishr, false, true  } };
   for (unsigned v = 0; v < ARRAY_SIZE(variants); v++) {
      nir_shader *nir = build_shift_variable(variants[v].op);
      struct r300_compute_admission adm;
      struct r300_compute_shift_variable_pattern sv = {0};
      struct r300_compute_shift_logical_pattern sl = {0};
      struct r300_compute_binary_map_pattern bm = {0};
      struct r300_compute_multilimb_mul_pattern mm = {0};
      prepare_detect_shader(nir);
      r300_nir_classify_compute(nir, &adm);
      CHECK(adm.admissible, "variable shift admits");
      r300_nir_detect_shift_variable(nir, &sv);
      CHECK(sv.is_shift_variable, "variable shift detected");
      CHECK(sv.is_left == variants[v].is_left, "variable shift records direction");
      CHECK(sv.is_arithmetic == variants[v].is_arith,
            "variable shift records signedness");
      CHECK(sv.input_a_ssbo_binding == 0 && sv.input_b_ssbo_binding == 1 &&
            sv.output_ssbo_binding == 2,
            "variable shift records bindings a=0 b=1 out=2");
      CHECK(sv.value_components == 1 && sv.value_bit_size == 32,
            "variable shift records scalar 32-bit");
      /* Disjointness from the verbs the dispatcher tries before it.  The
       * constant-shift detector needs one load; multilimb needs imul; binary_map
       * needs a commutative arithmetic op.  A two-load ishl/ushr matches none, so
       * the first-match router reaches shift_variable. */
      r300_nir_detect_shift_logical(nir, &sl);
      CHECK(!sl.is_shift_logical, "variable shift is not a constant shift");
      r300_nir_detect_multilimb_mul_pattern(nir, &mm);
      CHECK(!mm.is_multilimb_mul, "variable shift is not a multilimb multiply");
      r300_nir_detect_binary_map(nir, &bm);
      CHECK(!bm.is_binary_map, "variable shift is not a binary_map");
      ralloc_free(nir);
   }
}

static void
case_multitap_metadata(void)
{
   nir_shader *direct = build_multitap_box3_direct();
   struct r300_compute_multitap_gather_pattern dmap = {0};
   prepare_detect_shader(direct);
   r300_nir_detect_multitap_gather_pattern(direct, &dmap);
   CHECK(dmap.is_multitap_gather, "direct-offset box-3 gather detected");
   CHECK(dmap.tap_count == 3, "direct gather records 3 taps");
   CHECK(dmap.input_ssbo_binding == 0 && dmap.output_ssbo_binding == 1,
         "direct gather records constant bindings 0 -> 1");
   ralloc_free(direct);

   nir_shader *desc = build_multitap_box3_desc_base(false);
   struct r300_compute_multitap_gather_pattern smap = {0};
   prepare_detect_shader(desc);
   r300_nir_detect_multitap_gather_pattern(desc, &smap);
   CHECK(smap.is_multitap_gather,
         "descriptor-base box-3 gather detected (post-explicit_io shape)");
   CHECK(smap.tap_count == 3, "descriptor-base gather records 3 taps");
   ralloc_free(desc);

   nir_shader *bad = build_multitap_box3_desc_base(true);
   struct r300_compute_multitap_gather_pattern bmap = {0};
   prepare_detect_shader(bad);
   r300_nir_detect_multitap_gather_pattern(bad, &bmap);
   CHECK(!bmap.is_multitap_gather,
         "gid+2 tap rejected: the realized kernel is exactly box-3");
   ralloc_free(bad);
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

static void
case_qconj_metadata(void)
{
   nir_shader *nir = build_qconj_form(false);
   struct r300_compute_admission adm;
   struct r300_compute_qconj_pattern qc = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 qconj kernel admits");
   r300_nir_detect_qconj_pattern(nir, &qc);
   CHECK(qc.is_qconj, "qconj sign-flip shape detected");
   CHECK(qc.input_ssbo_binding == 0, "qconj metadata records input binding 0");
   CHECK(qc.output_ssbo_binding == 1, "qconj metadata records output binding 1");
   ralloc_free(nir);

   /* A conjugate that fails to negate one vector lane is the identity on that
    * lane, a different map; the exact-sign check must reject it. */
   nir_shader *bad = build_qconj_form(true);
   struct r300_compute_qconj_pattern bad_qc = {0};
   prepare_detect_shader(bad);
   r300_nir_detect_qconj_pattern(bad, &bad_qc);
   CHECK(!bad_qc.is_qconj, "qconj rejects an un-negated vector lane");
   ralloc_free(bad);

   /* A two-input kernel is not a unary conjugate; the single-load shape gate
    * must reject the binary-map form. */
   nir_shader *bin = build_binary_map_f32vec4();
   struct r300_compute_qconj_pattern bin_qc = {0};
   prepare_detect_shader(bin);
   r300_nir_detect_qconj_pattern(bin, &bin_qc);
   CHECK(!bin_qc.is_qconj, "qconj rejects a two-input kernel");
   ralloc_free(bin);
}

static void
case_qnorm_metadata(void)
{
   nir_shader *nir = build_qnorm_form();
   struct r300_compute_admission adm;
   struct r300_compute_qnorm_pattern qn = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 qnorm kernel admits");
   r300_nir_detect_qnorm_pattern(nir, &qn);
   CHECK(qn.is_qnorm, "qnorm self-dot splat shape detected");
   CHECK(qn.input_ssbo_binding == 0, "qnorm metadata records input binding 0");
   CHECK(qn.output_ssbo_binding == 1, "qnorm metadata records output binding 1");
   ralloc_free(nir);

   /* The conjugate splats no dot; it must not read as a squared norm. */
   nir_shader *conj = build_qconj_form(false);
   struct r300_compute_qnorm_pattern conj_qn = {0};
   prepare_detect_shader(conj);
   r300_nir_detect_qnorm_pattern(conj, &conj_qn);
   CHECK(!conj_qn.is_qnorm, "qnorm rejects a conjugate (no self-dot)");
   ralloc_free(conj);
}

/* Quaternion normalize: out = q * rsqrt(dot(q, q)), a one-load one-store kernel.
 * rsqrt is a scalar; nir_fmul broadcasts it to the vec4 via the .xxxx swizzle on
 * the rsqrt src operand -- the shape the qnormalize detector requires. */
static nir_shader *
build_qnormalize_form(void)
{
   nir_builder b = cs_builder("cs_qnormalize_f32vec4");
   nir_def *q = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0);
   nir_def *norm_sq = nir_fdot4(&b, q, q);
   nir_def *rsqrt   = nir_frsq(&b, norm_sq);
   nir_def *out     = nir_fmul(&b, q, rsqrt);
   nir_store_ssbo(&b, out, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static void
case_qnormalize_metadata(void)
{
   nir_shader *nir = build_qnormalize_form();
   struct r300_compute_qnormalize_pattern qn = {0};

   prepare_detect_shader(nir);
   r300_nir_detect_qnormalize_pattern(nir, &qn);
   CHECK(qn.is_qnormalize, "qnormalize: shape detected");
   CHECK(qn.input_ssbo_binding  == 0, "qnormalize: input binding 0");
   CHECK(qn.output_ssbo_binding == 1, "qnormalize: output binding 1");
   ralloc_free(nir);

   /* A two-input kernel is not a unary normalize; the qnormalize detector
    * must reject the binary-map form. */
   nir_shader *bin = build_binary_map_f32vec4();
   struct r300_compute_qnormalize_pattern bin_qn = {0};
   prepare_detect_shader(bin);
   r300_nir_detect_qnormalize_pattern(bin, &bin_qn);
   CHECK(!bin_qn.is_qnormalize, "qnormalize rejects a two-input kernel");
   ralloc_free(bin);
}

/* Quaternion fused multiply-add: out = q1*q2 + q3 (Hamilton product then vec4 add). */
static nir_shader *
build_qfmadd_form(void)
{
   nir_builder b = cs_builder("cs_qfmadd_f32vec4");
   nir_def *q1 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q2 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q3 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *x = nir_channel(&b, q2, 0), *y = nir_channel(&b, q2, 1);
   nir_def *z = nir_channel(&b, q2, 2), *w = nir_channel(&b, q2, 3);
   nir_def *nx = nir_fneg(&b, x), *ny = nir_fneg(&b, y);
   nir_def *nz = nir_fneg(&b, z), *nw = nir_fneg(&b, w);
   nir_def *pw = nir_vec4(&b, x, ny, nz, nw);
   nir_def *px = nir_vec4(&b, y, x, w, nz);
   nir_def *py = nir_vec4(&b, z, nw, x, y);
   nir_def *pz = nir_vec4(&b, w, z, ny, x);
   (void)nx;
   nir_def *prod = nir_vec4(&b, nir_fdot(&b, q1, pw), nir_fdot(&b, q1, px),
                            nir_fdot(&b, q1, py), nir_fdot(&b, q1, pz));
   nir_def *out = nir_fadd(&b, prod, q3);
   nir_store_ssbo(&b, out, nir_imm_int(&b, 3), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

/* Quaternion fused multiply-sub: out = q1*q2 - q3 (Hamilton product then vec4 sub). */
static nir_shader *
build_qfmsub_form(void)
{
   nir_builder b = cs_builder("cs_qfmsub_f32vec4");
   nir_def *q1 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q2 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q3 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *x = nir_channel(&b, q2, 0), *y = nir_channel(&b, q2, 1);
   nir_def *z = nir_channel(&b, q2, 2), *w = nir_channel(&b, q2, 3);
   nir_def *nx = nir_fneg(&b, x), *ny = nir_fneg(&b, y);
   nir_def *nz = nir_fneg(&b, z), *nw = nir_fneg(&b, w);
   nir_def *pw = nir_vec4(&b, x, ny, nz, nw);
   nir_def *px = nir_vec4(&b, y, x, w, nz);
   nir_def *py = nir_vec4(&b, z, nw, x, y);
   nir_def *pz = nir_vec4(&b, w, z, ny, x);
   (void)nx;
   nir_def *prod = nir_vec4(&b, nir_fdot(&b, q1, pw), nir_fdot(&b, q1, px),
                            nir_fdot(&b, q1, py), nir_fdot(&b, q1, pz));
   nir_def *out = nir_fsub(&b, prod, q3);
   nir_store_ssbo(&b, out, nir_imm_int(&b, 3), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

/* Quaternion fused triple product: out = (q1*q2)*q3 (two chained Hamilton products). */
static nir_shader *
build_qfmmul_form(void)
{
   nir_builder b = cs_builder("cs_qfmmul_f32vec4");
   nir_def *q1 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q2 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   nir_def *q3 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                               .align_mul = 16, .align_offset = 0);
   /* Inner product t = q1*q2 (eight-DP4 Hamilton product). */
   nir_def *x2 = nir_channel(&b, q2, 0), *y2 = nir_channel(&b, q2, 1);
   nir_def *z2 = nir_channel(&b, q2, 2), *w2 = nir_channel(&b, q2, 3);
   nir_def *ny2 = nir_fneg(&b, y2), *nz2 = nir_fneg(&b, z2), *nw2 = nir_fneg(&b, w2);
   nir_def *pw2 = nir_vec4(&b, x2, ny2, nz2, nw2);
   nir_def *px2 = nir_vec4(&b, y2, x2, w2, nz2);
   nir_def *py2 = nir_vec4(&b, z2, nw2, x2, y2);
   nir_def *pz2 = nir_vec4(&b, w2, z2, ny2, x2);
   nir_def *t = nir_vec4(&b, nir_fdot(&b, q1, pw2), nir_fdot(&b, q1, px2),
                         nir_fdot(&b, q1, py2), nir_fdot(&b, q1, pz2));
   /* Outer product out = t*q3 (second Hamilton product). */
   nir_def *x3 = nir_channel(&b, q3, 0), *y3 = nir_channel(&b, q3, 1);
   nir_def *z3 = nir_channel(&b, q3, 2), *w3 = nir_channel(&b, q3, 3);
   nir_def *ny3 = nir_fneg(&b, y3), *nz3 = nir_fneg(&b, z3), *nw3 = nir_fneg(&b, w3);
   nir_def *pw3 = nir_vec4(&b, x3, ny3, nz3, nw3);
   nir_def *px3 = nir_vec4(&b, y3, x3, w3, nz3);
   nir_def *py3 = nir_vec4(&b, z3, nw3, x3, y3);
   nir_def *pz3 = nir_vec4(&b, w3, z3, ny3, x3);
   nir_def *out = nir_vec4(&b, nir_fdot(&b, t, pw3), nir_fdot(&b, t, px3),
                           nir_fdot(&b, t, py3), nir_fdot(&b, t, pz3));
   nir_store_ssbo(&b, out, nir_imm_int(&b, 3), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static void
case_qfmadd_metadata(void)
{
   nir_shader *nir = build_qfmadd_form();
   struct r300_compute_qfmadd_pattern qfm = {0};

   prepare_detect_shader(nir);
   r300_nir_detect_qfmadd_pattern(nir, &qfm);
   CHECK(qfm.is_qfmadd, "qfmadd: shape detected");
   CHECK(!qfm.is_sub,   "qfmadd: is_sub is false");
   CHECK(qfm.input_a_ssbo_binding == 0, "qfmadd: input_a binding 0");
   CHECK(qfm.input_b_ssbo_binding == 1, "qfmadd: input_b binding 1");
   CHECK(qfm.input_c_ssbo_binding == 2, "qfmadd: input_c binding 2");
   CHECK(qfm.output_ssbo_binding  == 3, "qfmadd: output binding 3");
   ralloc_free(nir);
}

static void
case_qfmsub_metadata(void)
{
   nir_shader *nir = build_qfmsub_form();
   struct r300_compute_qfmadd_pattern qfm = {0};

   prepare_detect_shader(nir);
   r300_nir_detect_qfmadd_pattern(nir, &qfm);
   CHECK(qfm.is_qfmadd, "qfmsub: shape detected via qfmadd detector");
   CHECK(qfm.is_sub,    "qfmsub: is_sub is true");
   CHECK(qfm.input_a_ssbo_binding == 0, "qfmsub: input_a binding 0");
   CHECK(qfm.input_b_ssbo_binding == 1, "qfmsub: input_b binding 1");
   CHECK(qfm.input_c_ssbo_binding == 2, "qfmsub: input_c binding 2");
   CHECK(qfm.output_ssbo_binding  == 3, "qfmsub: output binding 3");

   /* QFMADD (fadd) must not fire the is_sub flag. */
   nir_shader *add = build_qfmadd_form();
   struct r300_compute_qfmadd_pattern add_qfm = {0};
   prepare_detect_shader(add);
   r300_nir_detect_qfmadd_pattern(add, &add_qfm);
   CHECK(add_qfm.is_qfmadd, "qfmsub: QFMADD form still admits");
   CHECK(!add_qfm.is_sub,   "qfmsub: QFMADD form has is_sub=false");
   ralloc_free(add);
   ralloc_free(nir);
}

static void
case_qfmmul_metadata(void)
{
   nir_shader *nir = build_qfmmul_form();
   struct r300_compute_qfmmul_pattern qfmm = {0};

   prepare_detect_shader(nir);
   r300_nir_detect_qfmmul_pattern(nir, &qfmm);
   CHECK(qfmm.is_qfmmul, "qfmmul: shape detected");
   CHECK(qfmm.input_a_ssbo_binding == 0, "qfmmul: input_a binding 0");
   CHECK(qfmm.input_b_ssbo_binding == 1, "qfmmul: input_b binding 1");
   CHECK(qfmm.input_c_ssbo_binding == 2, "qfmmul: input_c binding 2");
   CHECK(qfmm.output_ssbo_binding  == 3, "qfmmul: output binding 3");

   /* QFMADD (product + addend) must not match as triple product. */
   nir_shader *add = build_qfmadd_form();
   struct r300_compute_qfmmul_pattern add_qfmm = {0};
   prepare_detect_shader(add);
   r300_nir_detect_qfmmul_pattern(add, &add_qfmm);
   CHECK(!add_qfmm.is_qfmmul, "qfmmul rejects QFMADD (no inner product to find)");
   ralloc_free(add);
   ralloc_free(nir);
}

/* Constant-fill kernel: out_buffer[gid] = 0x42424242u (no loads). */
static nir_def *
const_fill_store_offset(nir_builder *b, unsigned element_bytes)
{
   return nir_imul(b, nir_load_global_invocation_index(b, 32),
                   nir_imm_int(b, (int)element_bytes));
}

static nir_shader *
build_const_fill_u32(void)
{
   nir_builder b = cs_builder("cs_const_fill_u32");
   nir_def *c = nir_imm_int(&b, 0x42424242);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_const_fill_constant_address(void)
{
   nir_builder b = cs_builder("cs_const_fill_constant_address");
   nir_def *c = nir_imm_int(&b, 0x42424242);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Constant-fill vec4 kernel: out_buffer[gid] = (0x01, 0x02, 0x03, 0x04) (no loads). */
static nir_shader *
build_const_fill_vec4(void)
{
   nir_builder b = cs_builder("cs_const_fill_vec4");
   nir_def *c = nir_imm_ivec4(&b, 0x01010101, 0x02020202, 0x03030303, 0x04040404);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 1), const_fill_store_offset(&b, 16),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_const_fill_narrow_slot(unsigned bit_size)
{
   nir_builder b =
      cs_builder(bit_size == 8 ? "cs_const_fill_u8_slot" :
                                  "cs_const_fill_u16_slot");
   nir_def *c = nir_imm_intN_t(&b, bit_size == 8 ? 0x7f : 0x1234,
                               bit_size);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = bit_size / 8,
                  .align_offset = 0);
   return b.shader;
}

static nir_def *
const_fill_flatten_offset(nir_builder *b, unsigned width, unsigned height,
                          bool include_z)
{
   nir_def *id = nir_load_global_invocation_id(b, 32);
   nir_def *x = nir_channel(b, id, 0);
   nir_def *y = nir_channel(b, id, 1);
   nir_def *flat =
      nir_iadd(b, nir_imul(b, y, nir_imm_int(b, (int)width)), x);

   if (include_z) {
      nir_def *z = nir_channel(b, id, 2);
      flat = nir_iadd(b, nir_imul(b, z,
                                  nir_imm_int(b, (int)(width * height))),
                      flat);
   }

   return nir_imul(b, flat, nir_imm_int(b, 4));
}

static nir_shader *
build_const_fill_flatten_2d(void)
{
   nir_builder b = cs_builder("cs_const_fill_flatten_2d");
   nir_def *c = nir_imm_int(&b, 0x42424242);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0),
                  const_fill_flatten_offset(&b, 16, 1, false),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_const_fill_flatten_3d(void)
{
   nir_builder b = cs_builder("cs_const_fill_flatten_3d");
   nir_def *c = nir_imm_int(&b, 0x11223344);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0),
                  const_fill_flatten_offset(&b, 16, 8, true),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_const_fill_metadata(void)
{
   /* Scalar uint32 constant fill: verify admission and detection metadata. */
   nir_shader *nir = build_const_fill_u32();
   struct r300_compute_admission adm;
   struct r300_compute_const_fill_pattern cf = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "const-fill u32 kernel admits");
   r300_nir_detect_const_fill_pattern(nir, &cf);
   CHECK(cf.is_const_fill, "const-fill u32 shape detected");
   CHECK(cf.value_components == 1, "const-fill records scalar width");
   CHECK(cf.value_bit_size == 32, "const-fill records 32-bit lane");
   /* 0x42424242 in LE bytes: R=0x42, G=0x42, B=0x42, A=0x42 */
   CHECK(cf.const_value[0] == 0x42 && cf.const_value[1] == 0x42 &&
         cf.const_value[2] == 0x42 && cf.const_value[3] == 0x42,
         "const-fill records RGBA8 bytes of 0x42424242");
   ralloc_free(nir);

   nir_shader *nir2d = build_const_fill_flatten_2d();
   struct r300_compute_const_fill_pattern cf2d = {0};
   struct r300_compute_index_pattern ip2d = {0};

   prepare_detect_shader(nir2d);
   r300_nir_detect_const_fill_pattern(nir2d, &cf2d);
   r300_nir_classify_index_consumption(nir2d, &ip2d);
   CHECK(cf2d.is_const_fill, "const-fill canonical 2D flatten detected");
   CHECK(ip2d.store_offset_valid && ip2d.store_offset_stride == 4 &&
         ip2d.store_offset_stride_y == 64 &&
         ip2d.store_offset_stride_z == 0,
         "const-fill canonical 2D flatten records output offset strides");
   ralloc_free(nir2d);

   nir_shader *nir3d = build_const_fill_flatten_3d();
   struct r300_compute_const_fill_pattern cf3d = {0};
   struct r300_compute_index_pattern ip3d = {0};

   prepare_detect_shader(nir3d);
   r300_nir_detect_const_fill_pattern(nir3d, &cf3d);
   r300_nir_classify_index_consumption(nir3d, &ip3d);
   CHECK(cf3d.is_const_fill, "const-fill canonical 3D flatten detected");
   CHECK(ip3d.store_offset_valid && ip3d.store_offset_stride == 4 &&
         ip3d.store_offset_stride_y == 64 &&
         ip3d.store_offset_stride_z == 512,
         "const-fill canonical 3D flatten records output offset strides");
   ralloc_free(nir3d);

   /* Vec4 constant fill is not replayable until all lanes are packed. */
   nir_shader *nir4 = build_const_fill_vec4();
   struct r300_compute_const_fill_pattern cf4 = {0};

   prepare_detect_shader(nir4);
   r300_nir_detect_const_fill_pattern(nir4, &cf4);
   CHECK(!cf4.is_const_fill, "const-fill vec4 rejects until lanes are packed");
   ralloc_free(nir4);

   /* Narrow scalar fills are not replayable until byte stride is explicit. */
   nir_shader *nir8 = build_const_fill_narrow_slot(8);
   struct r300_compute_const_fill_pattern cf8 = {0};

   prepare_detect_shader(nir8);
   r300_nir_detect_const_fill_pattern(nir8, &cf8);
   CHECK(!cf8.is_const_fill, "const-fill u8 rejects until byte stride is represented");
   ralloc_free(nir8);

   nir_shader *nir16 = build_const_fill_narrow_slot(16);
   struct r300_compute_const_fill_pattern cf16 = {0};

   prepare_detect_shader(nir16);
   r300_nir_detect_const_fill_pattern(nir16, &cf16);
   CHECK(!cf16.is_const_fill, "const-fill u16 rejects until byte stride is represented");
   ralloc_free(nir16);

   /* Discrimination: identity-map must NOT match CONSTFILL (it has a load). */
   nir_shader *ident = build_identity_map_f32vec4();
   struct r300_compute_const_fill_pattern cfi = {0};
   prepare_detect_shader(ident);
   r300_nir_detect_const_fill_pattern(ident, &cfi);
   CHECK(!cfi.is_const_fill, "const-fill rejects identity-map (has a load)");
   ralloc_free(ident);

   /* Discrimination: CONSTFILL must NOT match identity-map (no load). */
   nir_shader *cfn = build_const_fill_u32();
   struct r300_compute_identity_pattern ident2 = {0};
   prepare_detect_shader(cfn);
   r300_nir_detect_identity_map(cfn, &ident2);
   CHECK(!ident2.is_identity_map, "identity-map rejects const-fill (zero loads)");
   ralloc_free(cfn);

   /* Discrimination: unary-map must NOT match CONSTFILL. */
   nir_shader *cfn2 = build_const_fill_u32();
   struct r300_compute_unary_map_pattern um = {0};
   prepare_detect_shader(cfn2);
   r300_nir_detect_unary_map(cfn2, &um);
   CHECK(!um.is_unary_map, "unary-map rejects const-fill (zero loads)");
   ralloc_free(cfn2);
}

/* 0xDEADBEEF fill: all four bytes differ, exercising R/G/B/A channel ordering. */
static nir_shader *
build_const_fill_deadbeef(void)
{
   nir_builder b = cs_builder("cs_const_fill_deadbeef");
   nir_def *c = nir_imm_int(&b, 0xDEADBEEF);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* 0x00000000 fill: zero sentinel exercises the all-zero byte path. */
static nir_shader *
build_const_fill_zero(void)
{
   nir_builder b = cs_builder("cs_const_fill_zero");
   nir_def *c = nir_imm_int(&b, 0x00000000);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* 0xFFFFFFFF fill: saturation ceiling -- all bytes are 0xFF. */
static nir_shader *
build_const_fill_ff(void)
{
   nir_builder b = cs_builder("cs_const_fill_ff");
   nir_def *c = nir_imm_int(&b, 0xFFFFFFFF);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Fill at binding 7: non-default binding, verifies output_ssbo_binding capture. */
static nir_shader *
build_const_fill_binding7(void)
{
   nir_builder b = cs_builder("cs_const_fill_binding7");
   nir_def *c = nir_imm_int(&b, 0x11223344);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 7), const_fill_store_offset(&b, 4),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Negative: out[gid * 2] = C is not a contiguous const-fill replay. */
static nir_shader *
build_const_fill_strided_address(void)
{
   nir_builder b = cs_builder("cs_const_fill_strided_address");
   nir_def *c = nir_imm_int(&b, 0x01020304);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), const_fill_store_offset(&b, 8),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Negative: stored value is a load_ssbo result, not a load_const.
 * The detector's nir_def_is_const guard must reject this. */
static nir_shader *
build_const_fill_alu_stored(void)
{
   nir_builder b = cs_builder("cs_const_fill_alu_stored");
   nir_def *val = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                                .align_mul = 4, .align_offset = 0);
   nir_store_ssbo(&b, val, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Negative: two store_ssbo with constants.  collect_loads_stores counts nstore=2;
 * the nstore != 1 guard must reject this shape. */
static nir_shader *
build_const_fill_two_stores(void)
{
   nir_builder b = cs_builder("cs_const_fill_two_stores");
   nir_def *c = nir_imm_int(&b, 0x42424242);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_constfill_regression(void)
{
   /* --- Byte extraction: 0xDEADBEEF (all bytes distinct) ---
    * LE decomposition: R=byte0=0xEF, G=byte1=0xBE, B=byte2=0xAD, A=byte3=0xDE.
    * All four bytes differ so a channel-swap or mask truncation produces a
    * detectable mismatch. */
   {
      nir_shader *nir = build_const_fill_deadbeef();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(cf.is_const_fill, "deadbeef fill: shape detected");
      CHECK(cf.const_value[0] == 0xEF, "deadbeef fill: byte0 R=0xEF");
      CHECK(cf.const_value[1] == 0xBE, "deadbeef fill: byte1 G=0xBE");
      CHECK(cf.const_value[2] == 0xAD, "deadbeef fill: byte2 B=0xAD");
      CHECK(cf.const_value[3] == 0xDE, "deadbeef fill: byte3 A=0xDE");
      ralloc_free(nir);
   }

   /* --- Byte extraction: 0x00000000 (all-zero) --- */
   {
      nir_shader *nir = build_const_fill_zero();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(cf.is_const_fill, "zero fill: shape detected");
      CHECK(cf.const_value[0] == 0x00 && cf.const_value[1] == 0x00 &&
            cf.const_value[2] == 0x00 && cf.const_value[3] == 0x00,
            "zero fill: all bytes are 0x00");
      ralloc_free(nir);
   }

   /* --- Byte extraction: 0xFFFFFFFF (saturation ceiling) --- */
   {
      nir_shader *nir = build_const_fill_ff();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(cf.is_const_fill, "0xffffffff fill: shape detected");
      CHECK(cf.const_value[0] == 0xFF && cf.const_value[1] == 0xFF &&
            cf.const_value[2] == 0xFF && cf.const_value[3] == 0xFF,
            "0xffffffff fill: all bytes are 0xFF");
      ralloc_free(nir);
   }

   /* --- Binding capture at non-zero binding 7 --- */
   {
      nir_shader *nir = build_const_fill_binding7();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(cf.is_const_fill, "binding-7 fill: shape detected");
      CHECK(cf.output_ssbo_binding_valid, "binding-7 fill: binding captured");
      CHECK(cf.output_ssbo_binding == 7, "binding-7 fill: binding is 7");
      /* 0x11223344 LE: R=0x44, G=0x33, B=0x22, A=0x11 */
      CHECK(cf.const_value[0] == 0x44 && cf.const_value[1] == 0x33 &&
            cf.const_value[2] == 0x22 && cf.const_value[3] == 0x11,
            "binding-7 fill: byte extraction correct");
      ralloc_free(nir);
   }

   /* --- Negative: non-contiguous output byte offset. --- */
   {
      nir_shader *nir = build_const_fill_strided_address();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(!cf.is_const_fill, "strided-address fill: detector rejects");
      ralloc_free(nir);
   }

   /* --- Negative: a fixed output byte offset is not out[gid]. --- */
   {
      nir_shader *nir = build_const_fill_constant_address();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(!cf.is_const_fill, "constant-address fill: detector rejects");
      ralloc_free(nir);
   }

   /* --- Negative: stored value is a load_ssbo result, not a compile-time constant.
    * nir_def_is_const returns false for a load_ssbo def; the detector must reject. */
   {
      nir_shader *nir = build_const_fill_alu_stored();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(!cf.is_const_fill, "non-constant stored value: detector rejects");
      ralloc_free(nir);
   }

   /* --- Negative: two store_ssbo intrinsics.  nstore == 2 != 1; detector rejects. */
   {
      nir_shader *nir = build_const_fill_two_stores();
      struct r300_compute_const_fill_pattern cf = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_const_fill_pattern(nir, &cf);
      CHECK(!cf.is_const_fill, "two stores: detector rejects (nstore != 1)");
      ralloc_free(nir);
   }

   /* --- Discrimination: binary-map (two loads + one store) must NOT match CONSTFILL
    * (nload != 0 triggers the early return). */
   {
      nir_shader *bin = build_binary_map_f32vec4();
      struct r300_compute_const_fill_pattern cfb = {0};
      prepare_detect_shader(bin);
      r300_nir_detect_const_fill_pattern(bin, &cfb);
      CHECK(!cfb.is_const_fill, "binary-map rejected by const-fill (has loads)");

      /* Reciprocal: binary-map detector must NOT match a const-fill kernel
       * (zero loads, so neither input binding is a load_ssbo). */
      nir_shader *cfn3 = build_const_fill_deadbeef();
      struct r300_compute_binary_map_pattern bmcf = {0};
      prepare_detect_shader(cfn3);
      r300_nir_detect_binary_map(cfn3, &bmcf);
      CHECK(!bmcf.is_binary_map, "const-fill rejected by binary-map (zero loads)");
      ralloc_free(bin);
      ralloc_free(cfn3);
   }
}

static void
case_omul_metadata(void)
{
   nir_shader *nir = build_omul_form();
   struct r300_compute_admission adm;
   struct r300_compute_omul_pattern om = {0};

   prepare_detect_shader(nir);
   r300_nir_classify_compute(nir, &adm);
   CHECK(adm.admissible, "float4 omul kernel admits");
   r300_nir_detect_omul_pattern(nir, &om);
   CHECK(om.is_omul, "omul octonion-product shape detected");
   CHECK(om.input_a_ssbo_binding == 0, "omul records a binding 0");
   CHECK(om.input_b_ssbo_binding == 1, "omul records b binding 1");
   CHECK(om.input_c_ssbo_binding == 2, "omul records c binding 2");
   CHECK(om.input_d_ssbo_binding == 3, "omul records d binding 3");
   CHECK(om.output_lo_ssbo_binding == 4, "omul records o_lo binding 4");
   CHECK(om.output_hi_ssbo_binding == 5, "omul records o_hi binding 5");
   ralloc_free(nir);

   /* A plain Hamilton product (one store, two loads) is not an octonion
    * product; the four-load / two-store shape gate must reject it. */
   nir_shader *plain = build_qmul_form(false);
   struct r300_compute_omul_pattern om2 = {0};
   prepare_detect_shader(plain);
   r300_nir_detect_omul_pattern(plain, &om2);
   CHECK(!om2.is_omul, "omul rejects a plain Hamilton product");
   ralloc_free(plain);
}

static void
case_octonion_algebra_metadata(void)
{
   /* OADD and OSUB share the detector with an is_sub flag. */
   for (unsigned sub = 0; sub < 2; sub++) {
      nir_shader *nir = build_oaddsub_form(sub != 0);
      struct r300_compute_oaddsub_pattern p = {0};
      prepare_detect_shader(nir);
      r300_nir_detect_oaddsub_pattern(nir, &p);
      CHECK(p.is_oaddsub, sub ? "osub shape detected" : "oadd shape detected");
      CHECK(p.is_sub == (sub != 0), "oaddsub records the operator");
      CHECK(p.input_a_ssbo_binding == 0 && p.input_d_ssbo_binding == 3 &&
            p.output_lo_ssbo_binding == 4 && p.output_hi_ssbo_binding == 5,
            "oaddsub records its six bindings");
      ralloc_free(nir);
   }

   nir_shader *cj = build_oconj_form();
   struct r300_compute_oconj_pattern pc = {0};
   prepare_detect_shader(cj);
   r300_nir_detect_oconj_pattern(cj, &pc);
   CHECK(pc.is_oconj, "oconj (conj(a), -b) shape detected");
   CHECK(pc.input_a_ssbo_binding == 0 && pc.input_b_ssbo_binding == 1 &&
         pc.output_lo_ssbo_binding == 2 && pc.output_hi_ssbo_binding == 3,
         "oconj records its four bindings");
   ralloc_free(cj);

   nir_shader *nm = build_onorm_form();
   struct r300_compute_onorm_pattern pn = {0};
   prepare_detect_shader(nm);
   r300_nir_detect_onorm_pattern(nm, &pn);
   CHECK(pn.is_onorm, "onorm dot(a,a)+dot(b,b) shape detected");
   CHECK(pn.input_a_ssbo_binding == 0 && pn.input_b_ssbo_binding == 1 &&
         pn.output_ssbo_binding == 2, "onorm records its three bindings");
   ralloc_free(nm);

   /* Cross-rejection: the eight-wide octonion product is not an elementwise op. */
   nir_shader *om = build_omul_form();
   struct r300_compute_oaddsub_pattern oa = {0};
   struct r300_compute_oconj_pattern oc = {0};
   prepare_detect_shader(om);
   r300_nir_detect_oaddsub_pattern(om, &oa);
   r300_nir_detect_oconj_pattern(om, &oc);
   CHECK(!oa.is_oaddsub, "oaddsub rejects the octonion product");
   CHECK(!oc.is_oconj, "oconj rejects the octonion product");
   ralloc_free(om);
}

/* Index-consumption builders.  The classifier separates address-only index
 * use (carried by texel position at replay, 2048x2048 honest) from value use
 * (must materialize a * gid + b in FP24, bounded by the 2^17 exact-integer
 * ceiling at dispatch). */

/* out[gid] = in[gid]: the index feeds only load/store offsets. */
static nir_shader *
build_index_address_only(void)
{
   nir_builder b = cs_builder("cs_index_address_only");
   nir_def *off = nir_imul(&b, nir_load_global_invocation_index(&b, 32),
                           nir_imm_int(&b, 4));
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), off,
                               .align_mul = 4, .align_offset = 0);
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = gid: the linear index is the stored value (stride 1, offset 0). */
static nir_shader *
build_index_value_linear(void)
{
   nir_builder b = cs_builder("cs_index_value_linear");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, gid, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[0] = gid: affine value, but not a per-invocation output slot. */
static nir_shader *
build_index_value_constant_address(void)
{
   nir_builder b = cs_builder("cs_index_value_constant_address");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_store_ssbo(&b, gid, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = gid * 4 + 2: affine value consumption with captured stride. */
static nir_shader *
build_index_value_strided(void)
{
   nir_builder b = cs_builder("cs_index_value_strided");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *val = nir_iadd(&b, nir_imul(&b, gid, nir_imm_int(&b, 4)),
                           nir_imm_int(&b, 2));
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_index_value_float_nan_constant(void)
{
   nir_builder b = cs_builder("cs_index_value_float_nan_constant");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *val = nir_fadd(&b, nir_u2f32(&b, gid),
                           nir_imm_float(&b, NAN));
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = 0 * gid + 17: literal zero stride with an indexed destination. */
static nir_shader *
build_index_value_zero_stride(void)
{
   nir_builder b = cs_builder("cs_index_value_zero_stride");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *val = nir_iadd(&b, nir_imul(&b, gid, nir_imm_int(&b, 0)),
                           nir_imm_int(&b, 17));
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = gid * gid: no affine bound is derivable. */
static nir_shader *
build_index_value_general(void)
{
   nir_builder b = cs_builder("cs_index_value_general");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *val = nir_imul(&b, gid, gid);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[id.y] = in[id.y]: a vec3 id channel beyond .x feeds the address. */
static nir_shader *
build_index_coord_y(void)
{
   nir_builder b = cs_builder("cs_index_coord_y");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *off = nir_imul(&b, nir_channel(&b, id, 1), nir_imm_int(&b, 4));
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), off,
                               .align_mul = 4, .align_offset = 0);
   nir_store_ssbo(&b, in, nir_imm_int(&b, 1), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* The system-value lowering's real shape: (global_id + base_global_id).x as
 * the stored value, with the addressing chain off the same sum.  The base
 * intrinsic must register as the zero affine so the sum folds to identity. */
static nir_shader *
build_index_value_linear_with_base(void)
{
   nir_builder b = cs_builder("cs_index_value_linear_base");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *base = nir_load_base_global_invocation_id(&b, 32);
   nir_def *sum = nir_iadd(&b, id, base);
   nir_def *gid = nir_channel(&b, sum, 0);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, gid, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[flat] = flat with the canonical 3D flatten baked as constants:
 * flat = id.z * (16 * 8) + id.y * 16 + id.x.  Per-component affine must
 * report strides (1, 16, 128). */
static nir_shader *
build_index_value_flatten_3d(void)
{
   nir_builder b = cs_builder("cs_index_value_flatten_3d");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, id, 0);
   nir_def *y = nir_channel(&b, id, 1);
   nir_def *z = nir_channel(&b, id, 2);
   nir_def *flat = nir_iadd(&b,
                            nir_iadd(&b, nir_imul(&b, z, nir_imm_int(&b, 128)),
                                     nir_imul(&b, y, nir_imm_int(&b, 16))),
                            x);
   nir_def *off = nir_imul(&b, flat, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, flat, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = id.x + id.y through a swizzled vector add. */
static nir_shader *
build_index_value_swizzled_vector_sum(void)
{
   nir_builder b = cs_builder("cs_index_value_swizzled_vector_sum");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   const unsigned yxz[3] = { 1, 0, 2 };
   nir_def *sum = nir_iadd(&b, id, nir_swizzle(&b, id, yxz, 3));
   nir_def *val = nir_channel(&b, sum, 0);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = vec2(id.x, id.y).y: a vector constructor lane extraction. */
static nir_shader *
build_index_value_vec_lane_y(void)
{
   nir_builder b = cs_builder("cs_index_value_vec_lane_y");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *xy = nir_vec2(&b, nir_channel(&b, id, 0),
                          nir_channel(&b, id, 1));
   nir_def *val = nir_channel(&b, xy, 1);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[flat] = id.z * (16 * 8) + id.y * 16: no id.x term. */
static nir_shader *
build_index_value_yz_flatten_3d(void)
{
   nir_builder b = cs_builder("cs_index_value_yz_flatten_3d");
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *y = nir_channel(&b, id, 1);
   nir_def *z = nir_channel(&b, id, 2);
   nir_def *flat = nir_iadd(&b, nir_imul(&b, z, nir_imm_int(&b, 128)),
                            nir_imul(&b, y, nir_imm_int(&b, 16)));
   nir_def *off = nir_imul(&b, flat, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, flat, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = WorkGroupID.x: index-derived, but not the flat invocation id. */
static nir_shader *
build_index_value_workgroup_x(void)
{
   nir_builder b = cs_builder("cs_index_value_workgroup_x");
   nir_def *wg = nir_load_workgroup_id(&b);
   nir_def *val = nir_channel(&b, wg, 0);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* out[gid] = LocalInvocationIndex: index-derived, but not the flat id. */
static nir_shader *
build_index_value_local_index(void)
{
   nir_builder b = cs_builder("cs_index_value_local_index");
   nir_def *val = nir_load_local_invocation_index(&b);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_index_value_global_id_deref(void)
{
   nir_builder b = cs_builder("cs_index_value_global_id_deref");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_variable *global_id =
      nir_variable_create(b.shader, nir_var_system_value, glsl_uvec_type(3),
                          "gl_GlobalInvocationID");
   global_id->data.location = SYSTEM_VALUE_GLOBAL_INVOCATION_ID;

   nir_def *id = nir_channel(&b, nir_load_var(&b, global_id), 0);
   nir_def *off = nir_imul(&b, id, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, id, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_index_value_global_plus_zero_local(void)
{
   nir_builder b = cs_builder("cs_index_value_global_plus_zero_local");
   nir_def *id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *local_zero =
      nir_imul(&b, nir_load_local_invocation_index(&b), nir_imm_int(&b, 0));
   nir_def *val = nir_iadd(&b, id, local_zero);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static nir_shader *
build_index_value_global_plus_zero_workgroup(void)
{
   nir_builder b = cs_builder("cs_index_value_global_plus_zero_workgroup");
   nir_def *id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *workgroup_x = nir_channel(&b, nir_load_workgroup_id(&b), 0);
   nir_def *workgroup_zero = nir_imul(&b, workgroup_x, nir_imm_int(&b, 0));
   nir_def *val = nir_iadd(&b, id, workgroup_zero);
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, val, nir_imm_int(&b, 0), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static unsigned
count_intrinsic(nir_shader *shader, nir_intrinsic_op op)
{
   unsigned count = 0;

   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == op)
               count++;
         }
      }
   }

   return count;
}

static void
case_index_consumption(void)
{
   printf("index-consumption classifier\n");

   nir_shader *none = build_const_fill_constant_address();
   struct r300_compute_index_pattern p = {0};
   r300_nir_classify_index_consumption(none, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_NONE,
         "const-fill kernel classifies INDEX_NONE");
   ralloc_free(none);

   nir_shader *addr = build_index_address_only();
   r300_nir_classify_index_consumption(addr, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_ADDRESS_ONLY,
         "offset-only index use classifies ADDRESS_ONLY");
   CHECK(!p.uses_component_y && !p.uses_component_z,
         "scalar invocation index reads no y/z channel");
   ralloc_free(addr);

   nir_shader *lin = build_index_value_linear();
   r300_nir_classify_index_consumption(lin, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "stored index classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 1 && p.offset == 0,
         "stored index captures stride 1 offset 0");
   CHECK(p.affine_global_invocation_only,
         "stored global index keeps AFFINE_IOTA source identity");
   CHECK(p.store_offset_valid && p.store_offset_global_invocation_only &&
         p.store_offset_stride == 4 && p.store_offset_offset == 0,
         "stored global index records out[gid] byte offset");
   ralloc_free(lin);

   nir_shader *fixed_addr = build_index_value_constant_address();
   r300_nir_classify_index_consumption(fixed_addr, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "constant-address iota value still classifies VALUE_AFFINE");
   CHECK(!p.store_offset_valid,
         "constant-address iota value has no tracked out[gid] offset");
   ralloc_free(fixed_addr);

   nir_shader *str = build_index_value_strided();
   r300_nir_classify_index_consumption(str, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "gid*4+2 classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 4 && p.offset == 2,
         "gid*4+2 captures stride 4 offset 2");
   ralloc_free(str);

   nir_shader *nan = build_index_value_float_nan_constant();
   r300_nir_classify_index_consumption(nan, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_GENERAL,
         "non-finite float constant rejects as VALUE_GENERAL");
   ralloc_free(nan);

   nir_shader *zero_stride = build_index_value_zero_stride();
   r300_nir_classify_index_consumption(zero_stride, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "zero-stride value classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 0 && p.offset == 17,
         "zero-stride value preserves literal stride 0");
   CHECK(p.store_offset_valid && p.store_offset_stride == 4 &&
         p.store_offset_offset == 0,
         "zero-stride value records out[gid] byte offset");
   ralloc_free(zero_stride);

   nir_shader *gen = build_index_value_general();
   r300_nir_classify_index_consumption(gen, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_GENERAL,
         "gid*gid classifies VALUE_GENERAL");
   ralloc_free(gen);

   nir_shader *wb = build_index_value_linear_with_base();
   r300_nir_classify_index_consumption(wb, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "(global_id + zero base).x stored classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 1 && p.offset == 0,
         "zero base folds to identity affine");
   CHECK(p.affine_global_invocation_only,
         "zero base preserves global invocation source identity");
   ralloc_free(wb);

   nir_shader *cy = build_index_coord_y();
   r300_nir_classify_index_consumption(cy, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_ADDRESS_ONLY,
         "vec3 id channel feeding the address classifies ADDRESS_ONLY");
   CHECK(p.uses_component_y, "channel .y consumption is reported");
   ralloc_free(cy);

   nir_shader *f3 = build_index_value_flatten_3d();
   r300_nir_classify_index_consumption(f3, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE_3D,
         "canonical 3D flatten classifies VALUE_AFFINE_3D");
   CHECK(p.stride_valid && p.stride == 1 && p.stride_y == 16 &&
         p.stride_z == 128 && p.offset == 0,
         "3D flatten captures strides (1, 16, 128)");
   struct r300_compute_affine_iota_pattern it3 = {0};
   r300_nir_detect_affine_iota_pattern(f3, &it3);
   CHECK(it3.is_affine_iota && it3.stride == 1 && it3.stride_y == 16 &&
         it3.stride_z == 128,
         "affine-iota detector carries the 3D strides");
   ralloc_free(f3);

   nir_shader *swz = build_index_value_swizzled_vector_sum();
   r300_nir_classify_index_consumption(swz, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_GENERAL,
         "swizzled vector affine sum rejects as VALUE_GENERAL");
   ralloc_free(swz);

   nir_shader *lane = build_index_value_vec_lane_y();
   r300_nir_classify_index_consumption(lane, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_GENERAL,
         "vec constructor lane extraction rejects as VALUE_GENERAL");
   ralloc_free(lane);

   nir_shader *yz = build_index_value_yz_flatten_3d();
   r300_nir_classify_index_consumption(yz, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE_3D,
         "3D flatten without x stride classifies VALUE_AFFINE_3D");
   CHECK(p.stride_valid && p.stride == 0 && p.stride_y == 16 &&
         p.stride_z == 128 && p.offset == 0,
         "3D flatten without x stride preserves stride 0");
   CHECK(p.store_offset_valid && p.store_offset_stride == 0 &&
         p.store_offset_stride_y == 64 && p.store_offset_stride_z == 512 &&
         p.store_offset_offset == 0,
         "3D flatten without x stride records y/z-only byte offset");
   struct r300_compute_affine_iota_pattern it_yz = {0};
   r300_nir_detect_affine_iota_pattern(yz, &it_yz);
   CHECK(!it_yz.is_affine_iota,
         "y/z-only output address rejects affine-iota");
   ralloc_free(yz);

   nir_shader *wg = build_index_value_workgroup_x();
   r300_nir_classify_index_consumption(wg, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "workgroup id value still classifies as affine index-derived");
   CHECK(!p.affine_global_invocation_only,
         "workgroup id value is not AFFINE_IOTA source identity");
   ralloc_free(wg);

   nir_shader *local = build_index_value_local_index();
   r300_nir_classify_index_consumption(local, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "local index value still classifies as affine index-derived");
   CHECK(!p.affine_global_invocation_only,
         "local index value is not AFFINE_IOTA source identity");
   ralloc_free(local);

   nir_shader *zero_local = build_index_value_global_plus_zero_local();
   r300_nir_classify_index_consumption(zero_local, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "zeroed local term still classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 1 && p.offset == 0,
         "zeroed local term preserves global stride 1");
   CHECK(p.affine_global_invocation_only,
         "zeroed local term is source-free for AFFINE_IOTA");
   ralloc_free(zero_local);

   nir_shader *zero_workgroup = build_index_value_global_plus_zero_workgroup();
   r300_nir_classify_index_consumption(zero_workgroup, &p);
   CHECK(p.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE,
         "zeroed workgroup term still classifies VALUE_AFFINE");
   CHECK(p.stride_valid && p.stride == 1 && p.offset == 0,
         "zeroed workgroup term preserves global stride 1");
   CHECK(p.affine_global_invocation_only,
         "zeroed workgroup term is source-free for AFFINE_IOTA");
   ralloc_free(zero_workgroup);

   /* FP24 exact-ceiling guard boundaries (pure arithmetic, no NIR). */
   CHECK(r300_grid_linear_index_exact(131072),
         "linear total 2^17 admits (largest index 2^17 - 1)");
   CHECK(!r300_grid_linear_index_exact(131073),
         "linear total 2^17 + 1 rejects");
   CHECK(r300_grid_strided_index_exact(32768, 4, 0),
         "strided 4 * (2^15 - 1) admits");
   CHECK(!r300_grid_strided_index_exact(32770, 4, 0),
         "strided 4 * (2^15 + 1) rejects");
   CHECK(r300_grid_strided_index_exact(2048u * 2048u, 0, 17),
         "zero-stride affine admits representable constant value");
   CHECK(!r300_grid_strided_index_exact(1, 0,
                                        R300_FP24_EXACT_INT_CEILING + 1),
         "zero-stride affine rejects unrepresentable constant value");
   CHECK(r300_grid_index_exact(R300_GRID_INDEX_NONE, 2048u * 2048u, 0, 0),
         "position-addressed 2048x2048 fold admits");
   CHECK(!r300_grid_index_exact(R300_GRID_INDEX_LINEAR, 2048u * 2048u, 0, 0),
         "linear-indexed 2048x2048 fold rejects");
}

static struct r300_compute_affine_iota_pattern
detect_affine_iota(nir_shader *shader)
{
   struct r300_compute_affine_iota_pattern pattern = {0};
   r300_nir_detect_affine_iota_pattern(shader, &pattern);
   return pattern;
}

static void
case_compute_global_id_system_value_lowering(void)
{
   printf("compute global-id system-value lowering\n");

   static const nir_shader_compiler_options plain_options;

   nir_shader *plain = build_index_value_global_id_deref();
   plain->options = &plain_options;
   CHECK(count_intrinsic(plain, nir_intrinsic_load_deref) > 0,
         "pre-lowering compute global-id uses a system-value deref");
   NIR_PASS(_, plain, nir_lower_system_values);
   CHECK(count_intrinsic(plain, nir_intrinsic_load_deref) == 0,
         "plain system-value lowering removes the global-id deref");
   CHECK(count_intrinsic(plain, nir_intrinsic_load_global_invocation_id) > 0,
         "plain system-value lowering emits compute global-id intrinsic");
   CHECK(count_intrinsic(plain, nir_intrinsic_load_local_invocation_id) == 0 &&
         count_intrinsic(plain, nir_intrinsic_load_workgroup_id) == 0,
         "plain system-value lowering avoids local/workgroup decomposition");
   struct r300_compute_index_pattern plain_index = {0};
   r300_nir_classify_index_consumption(plain, &plain_index);
   CHECK(plain_index.affine_global_invocation_only,
         "plain system-value lowering keeps affine-iota source identity");
   struct r300_compute_affine_iota_pattern plain_iota =
      detect_affine_iota(plain);
   CHECK(plain_iota.is_affine_iota && plain_iota.stride == 1 &&
         plain_iota.offset == 0,
         "plain system-value lowering detects affine-iota");
   ralloc_free(plain);
}

static void
case_affine_iota(void)
{
   printf("affine-iota detector\n");

   nir_shader *lin = build_index_value_linear();
   struct r300_compute_affine_iota_pattern it = detect_affine_iota(lin);
   CHECK(it.is_affine_iota, "out[gid] = gid detects affine-iota");
   CHECK(it.stride == 1 && it.offset == 0,
         "iota captures stride 1 offset 0");
   ralloc_free(lin);

   nir_shader *fixed_addr = build_index_value_constant_address();
   it = detect_affine_iota(fixed_addr);
   CHECK(!it.is_affine_iota, "out[0] = gid rejects affine-iota");
   ralloc_free(fixed_addr);

   nir_shader *str = build_index_value_strided();
   it = detect_affine_iota(str);
   CHECK(it.is_affine_iota, "out[gid] = gid*4+2 detects affine-iota");
   CHECK(it.stride == 4 && it.offset == 2,
         "iota captures stride 4 offset 2");
   ralloc_free(str);

   nir_shader *zero_stride = build_index_value_zero_stride();
   it = detect_affine_iota(zero_stride);
   CHECK(it.is_affine_iota && it.stride == 0 && it.offset == 17,
         "out[gid] = 0*gid+17 detects affine-iota");
   ralloc_free(zero_stride);

   nir_shader *gen = build_index_value_general();
   it = detect_affine_iota(gen);
   CHECK(!it.is_affine_iota, "gid*gid rejects affine-iota");
   ralloc_free(gen);

   nir_shader *addr = build_index_address_only();
   it = detect_affine_iota(addr);
   CHECK(!it.is_affine_iota, "load-bearing identity shape rejects affine-iota");
   ralloc_free(addr);

   nir_shader *cf = build_const_fill_constant_address();
   it = detect_affine_iota(cf);
   CHECK(!it.is_affine_iota, "const-fill (no index) rejects affine-iota");
   ralloc_free(cf);

   nir_shader *wg = build_index_value_workgroup_x();
   it = detect_affine_iota(wg);
   CHECK(!it.is_affine_iota, "workgroup id value rejects affine-iota");
   ralloc_free(wg);

   nir_shader *local = build_index_value_local_index();
   it = detect_affine_iota(local);
   CHECK(!it.is_affine_iota, "local index value rejects affine-iota");
   ralloc_free(local);

   nir_shader *zero_local = build_index_value_global_plus_zero_local();
   it = detect_affine_iota(zero_local);
   CHECK(it.is_affine_iota && it.stride == 1 && it.offset == 0,
         "zeroed local term detects affine-iota");
   ralloc_free(zero_local);

   nir_shader *zero_workgroup = build_index_value_global_plus_zero_workgroup();
   it = detect_affine_iota(zero_workgroup);
   CHECK(it.is_affine_iota && it.stride == 1 && it.offset == 0,
         "zeroed workgroup term detects affine-iota");
   ralloc_free(zero_workgroup);
}

/* Q16.16 fixed-point addition: out = a + b in 2-limb carry form.
 *   lo_sum = iadd(iand(a, 0xFFFF), iand(b, 0xFFFF))
 *   carry  = ushr(lo_sum, 16)
 *   hi_sum = iadd(iadd(ushr(a, 16), ushr(b, 16)), carry)
 *   out    = ior(ishl(hi_sum, 16), iand(lo_sum, 0xFFFF))
 * All intermediates <= 2^17 - 1 (FP24 exact-integer range). */
static nir_shader *
build_q16_16_add_u32(void)
{
   nir_builder b = cs_builder("cs_q16_16_add_u32");
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *bv = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                               .align_mul = 4, .align_offset = 0);
   nir_def *lo_a    = nir_iand_imm(&b, a, 0xFFFF);
   nir_def *lo_b    = nir_iand_imm(&b, bv, 0xFFFF);
   nir_def *lo_sum  = nir_iadd(&b, lo_a, lo_b);
   nir_def *carry   = nir_ushr_imm(&b, lo_sum, 16);
   nir_def *lo_out  = nir_iand_imm(&b, lo_sum, 0xFFFF);
   nir_def *hi_a    = nir_ushr_imm(&b, a, 16);
   nir_def *hi_b    = nir_ushr_imm(&b, bv, 16);
   nir_def *hi_pair = nir_iadd(&b, hi_a, hi_b);
   nir_def *hi_sum  = nir_iadd(&b, hi_pair, carry);
   nir_def *hi_out  = nir_ishl_imm(&b, hi_sum, 16);
   nir_def *out     = nir_ior(&b, hi_out, lo_out);
   nir_store_ssbo(&b, out, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_q16_16_add(void)
{
   printf("Q16_16_ADD detector\n");

   nir_shader *nir = build_q16_16_add_u32();
   struct r300_compute_q16_16_add_pattern qa = {0};
   r300_nir_detect_q16_16_add_pattern(nir, &qa);
   CHECK(qa.is_q16_16_add, "q16_16_add: 2-limb carry shape detected");
   CHECK(qa.input_a_ssbo_binding == 0, "q16_16_add: input_a binding 0");
   CHECK(qa.input_b_ssbo_binding == 1, "q16_16_add: input_b binding 1");
   CHECK(qa.output_ssbo_binding  == 2, "q16_16_add: output binding 2");
   ralloc_free(nir);

   /* A plain iadd (BINARY_MAP shape) must not fire the Q16_16_ADD detector.
    * Distinguishes the carry form from a bare elementwise add. */
   nir_shader *plain = build_binary_map_f32vec4();
   struct r300_compute_q16_16_add_pattern plain_qa = {0};
   r300_nir_detect_q16_16_add_pattern(plain, &plain_qa);
   CHECK(!plain_qa.is_q16_16_add, "q16_16_add: plain iadd rejects (no carry chain)");
   ralloc_free(plain);

   /* A plain u32 imul must not fire: it has no iand/ushr/ior carry structure. */
   {
      nir_builder nb = cs_builder("cs_q16_16_add_reject_imul");
      nir_def *ra = nir_load_ssbo(&nb, 1, 32, nir_imm_int(&nb, 0), nir_imm_int(&nb, 0),
                                  .align_mul = 4, .align_offset = 0);
      nir_def *rb = nir_load_ssbo(&nb, 1, 32, nir_imm_int(&nb, 1), nir_imm_int(&nb, 0),
                                  .align_mul = 4, .align_offset = 0);
      nir_def *rprod = nir_imul(&nb, ra, rb);
      nir_store_ssbo(&nb, rprod, nir_imm_int(&nb, 2), nir_imm_int(&nb, 0),
                     .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
      struct r300_compute_q16_16_add_pattern mul_qa = {0};
      r300_nir_detect_q16_16_add_pattern(nb.shader, &mul_qa);
      CHECK(!mul_qa.is_q16_16_add, "q16_16_add: imul kernel rejects (no carry chain)");
      ralloc_free(nb.shader);
   }
}

/* out[gid] = a[gid] * b[gid] for u32: the multilimb-multiply shape. */
static nir_shader *
build_multilimb_mul_u32(void)
{
   nir_builder b = cs_builder("cs_multilimb_mul_u32");
   nir_def *a = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *c = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 4, .align_offset = 0);
   nir_def *prod = nir_imul(&b, a, c);
   nir_store_ssbo(&b, prod, nir_imm_int(&b, 2), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_multilimb_mul(void)
{
   printf("multilimb-mul detector\n");
   struct r300_compute_multilimb_mul_pattern mm = {0};

   nir_shader *mul = build_multilimb_mul_u32();
   r300_nir_detect_multilimb_mul_pattern(mul, &mm);
   CHECK(mm.is_multilimb_mul, "u32 imul of two loads detects multilimb");
   CHECK(mm.input_a_ssbo_binding == 0 && mm.input_b_ssbo_binding == 1 &&
         mm.output_ssbo_binding == 2,
         "multilimb records its three bindings");
   /* The same kernel also matches binary-map imul; the pipeline clears the
    * elementwise route when multilimb fires.  Pin that both detect. */
   struct r300_compute_binary_map_pattern bm = {0};
   r300_nir_detect_binary_map(mul, &bm);
   CHECK(bm.is_binary_map, "binary-map also detects the imul shape");
   ralloc_free(mul);

   nir_shader *add = build_binary_map_f32vec4();
   r300_nir_detect_multilimb_mul_pattern(add, &mm);
   CHECK(!mm.is_multilimb_mul, "vec4 fadd rejects multilimb");
   ralloc_free(add);

   nir_shader *iota = build_index_value_linear();
   r300_nir_detect_multilimb_mul_pattern(iota, &mm);
   CHECK(!mm.is_multilimb_mul, "zero-load iota shape rejects multilimb");
   ralloc_free(iota);
}

enum log4_pool_variant {
   LOG4_POOL_BASELINE,
   LOG4_POOL_MIXED_INPUT_BINDING,
   LOG4_POOL_INPUT_CONSTANT_BASE,
   LOG4_POOL_OUTPUT_CONSTANT_BASE,
   LOG4_POOL_VECTOR_LOAD,
};

static nir_def *
log4_iadd_scalar(nir_builder *b, nir_def *a, unsigned a_chan, nir_def *c,
                 unsigned c_chan)
{
   nir_alu_instr *alu = nir_alu_instr_create(b->shader, nir_op_iadd);
   alu->src[0].src = nir_src_for_ssa(a);
   alu->src[0].swizzle[0] = a_chan;
   alu->src[1].src = nir_src_for_ssa(c);
   alu->src[1].swizzle[0] = c_chan;
   nir_def_init(&alu->instr, &alu->def, 1, 32);
   nir_builder_instr_insert(b, &alu->instr);
   return &alu->def;
}

static nir_def *
log4_byte_offset(nir_builder *b, unsigned row_w, unsigned delta,
                 unsigned base)
{
   nir_def *id = nir_load_global_invocation_id(b, 32);
   nir_def *x = nir_channel(b, id, 0);
   nir_def *y = nir_channel(b, id, 1);
   nir_def *x_bytes = nir_imul(b, x, nir_imm_int(b, 8));
   nir_def *y_bytes = nir_imul(b, y, nir_imm_int(b, (int)(row_w * 8u)));
   nir_def *off = nir_iadd(b, x_bytes, y_bytes);
   if (delta || base)
      off = nir_iadd_imm(b, off, (int)(delta + base));
   return off;
}

static nir_def *
log4_store_offset(nir_builder *b, unsigned row_w, unsigned base)
{
   nir_def *id = nir_load_global_invocation_id(b, 32);
   nir_def *x = nir_channel(b, id, 0);
   nir_def *y = nir_channel(b, id, 1);
   nir_def *x_bytes = nir_imul(b, x, nir_imm_int(b, 4));
   nir_def *y_bytes = nir_imul(b, y, nir_imm_int(b, (int)(2u * row_w)));
   nir_def *off = nir_iadd(b, x_bytes, y_bytes);
   if (base)
      off = nir_iadd_imm(b, off, (int)base);
   return off;
}

static nir_shader *
build_log4_pool_variant(enum log4_pool_variant variant)
{
   nir_builder b = cs_builder("cs_log4_pool");
   const unsigned row_w = 8;
   const unsigned half_row = row_w * 4u;
   const unsigned input_base =
      variant == LOG4_POOL_INPUT_CONSTANT_BASE ? 4u : 0u;
   const unsigned output_base =
      variant == LOG4_POOL_OUTPUT_CONSTANT_BASE ? 4u : 0u;
   const unsigned deltas[4] = { 0, 4, half_row, half_row + 4 };
   nir_def *load[4];

   for (unsigned i = 0; i < 4; i++) {
      const int binding_index =
         variant == LOG4_POOL_MIXED_INPUT_BINDING && i == 3 ? 2 : 0;
      const unsigned components =
         variant == LOG4_POOL_VECTOR_LOAD && i == 0 ? 4 : 1;
      nir_def *binding = nir_imm_int(&b, binding_index);
      load[i] = nir_load_ssbo(&b, components,
                              32, binding,
                              log4_byte_offset(&b, row_w, deltas[i],
                                               input_base),
                              .align_mul = 4, .align_offset = 0);
   }

   nir_def *sum_lo = log4_iadd_scalar(&b, load[0], 0, load[1], 0);
   nir_def *sum_hi = log4_iadd_scalar(&b, load[2], 0, load[3], 0);
   nir_def *sum = log4_iadd_scalar(&b, sum_lo, 0, sum_hi, 0);
   sum = nir_iadd_imm(&b, sum, 2);
   nir_def *avg = nir_ushr(&b, sum, nir_imm_int(&b, 2));
   nir_store_ssbo(&b, avg, nir_imm_int(&b, 1),
                  log4_store_offset(&b, row_w, output_base),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_log4_pool(void)
{
   printf("log4 average-pool detector\n");
   struct r300_compute_log4_pool_pattern lp = {0};

   nir_shader *base = build_log4_pool_variant(LOG4_POOL_BASELINE);
   r300_nir_detect_log4_pool_pattern(base, &lp);
   CHECK(lp.is_log4_pool, "log4 scalar 2x2 average detects");
   CHECK(lp.row_w == 8 && lp.input_binding_valid &&
         lp.input_ssbo_binding == 0 && lp.output_binding_valid &&
         lp.output_ssbo_binding == 1,
         "log4 captures row width and constant bindings");
   ralloc_free(base);

   nir_shader *mixed = build_log4_pool_variant(LOG4_POOL_MIXED_INPUT_BINDING);
   r300_nir_detect_log4_pool_pattern(mixed, &lp);
   CHECK(!lp.is_log4_pool, "log4 mixed input SSBO bindings reject");
   ralloc_free(mixed);

   nir_shader *input_base =
      build_log4_pool_variant(LOG4_POOL_INPUT_CONSTANT_BASE);
   r300_nir_detect_log4_pool_pattern(input_base, &lp);
   CHECK(!lp.is_log4_pool, "log4 nonzero input byte base rejects");
   ralloc_free(input_base);

   nir_shader *output_base =
      build_log4_pool_variant(LOG4_POOL_OUTPUT_CONSTANT_BASE);
   r300_nir_detect_log4_pool_pattern(output_base, &lp);
   CHECK(!lp.is_log4_pool, "log4 nonzero output byte base rejects");
   ralloc_free(output_base);

   nir_shader *vector = build_log4_pool_variant(LOG4_POOL_VECTOR_LOAD);
   r300_nir_detect_log4_pool_pattern(vector, &lp);
   CHECK(!lp.is_log4_pool, "log4 swizzled vector SSBO load rejects");
   ralloc_free(vector);
}

/* old = atomicCompSwap(g[gid], 0xDEADBEEF, 0x00FF10AA); r[gid] = old. */
static nir_shader *
build_cas_const_u32(bool const_operands)
{
   nir_builder b = cs_builder("cs_cas_const_u32");
   nir_def *gid = nir_load_global_invocation_index(&b, 32);
   nir_def *off = nir_imul(&b, gid, nir_imm_int(&b, 4));
   nir_intrinsic_instr *swap =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_ssbo_atomic_swap);
   swap->num_components = 1;
   nir_def_init(&swap->instr, &swap->def, 1, 32);
   nir_intrinsic_set_atomic_op(swap, nir_atomic_op_cmpxchg);
   swap->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
   swap->src[1] = nir_src_for_ssa(off);
   swap->src[2] = nir_src_for_ssa(const_operands ? nir_imm_int(&b, 0xDEADBEEF)
                                                 : gid);
   swap->src[3] = nir_src_for_ssa(nir_imm_int(&b, 0x00FF10AA));
   nir_builder_instr_insert(&b, &swap->instr);
   nir_store_ssbo(&b, &swap->def, nir_imm_int(&b, 1), off,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_cas(void)
{
   printf("constant-operand CAS detector\n");
   struct r300_compute_cas_pattern cp = {0};

   nir_shader *cas = build_cas_const_u32(true);
   r300_nir_detect_cas_pattern(cas, &cp);
   CHECK(cp.is_cas, "constant-operand comp_swap detects CAS");
   CHECK(cp.expect == 0xDEADBEEF && cp.value_new == 0x00FF10AA,
         "CAS captures expect and new");
   CHECK(cp.guard_binding_valid && cp.guard_ssbo_binding == 0 &&
         cp.result_binding_valid && cp.result_ssbo_binding == 1,
         "CAS records guard and result bindings");
   struct r300_compute_admission adm;
   r300_nir_classify_compute(cas, &adm);
   CHECK(adm.admissible, "constant-operand comp_swap ADMITS classification");
   ralloc_free(cas);

   nir_shader *gen = build_cas_const_u32(false);
   r300_nir_detect_cas_pattern(gen, &cp);
   CHECK(!cp.is_cas, "non-constant compare rejects CAS");
   r300_nir_classify_compute(gen, &adm);
   CHECK(!adm.admissible &&
         adm.reason == R300_COMPUTE_REJECT_GENERAL_ATOMIC,
         "non-constant comp_swap still rejects GENERAL_ATOMIC");
   ralloc_free(gen);
}

/* ---- storage-image RT-export detector ---- */

/* Single 2D image, or an array-of-images variable (descriptor array).  The
 * array form is a glsl_array_type of a non-array 2D image -- not a 2D-array
 * image dimension -- so the RT-export detector rejects on the array type and
 * the array deref index. */
static nir_variable *
cs_image2d_var(nir_builder *b, const char *name, unsigned binding,
               enum pipe_format format, bool array_of_images)
{
   const struct glsl_type *img =
      glsl_image_type(GLSL_SAMPLER_DIM_2D, false, GLSL_TYPE_FLOAT);
   if (array_of_images)
      img = glsl_array_type(img, 2, 0);
   nir_variable *var =
      nir_variable_create(b->shader, nir_var_image, img, name);
   var->data.binding = binding;
   var->data.explicit_binding = true;
   var->data.image.format = format;
   var->data.access = ACCESS_NON_READABLE;
   return var;
}

/* Positive: one image_deref_store of RGBA8 at gid.xy. */
static nir_shader *
build_rt_export_ok(void)
{
   nir_builder b = cs_builder("cs_rt_export_ok");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R8G8B8A8_UNORM, false);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *coord2 = nir_trim_vector(&b, gid, 2);
   /* image_*_store coordinate is always 4 components in NIR. */
   nir_def *coord = nir_pad_vector_imm_int(&b, coord2, 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: constant coordinate (no gid). */
static nir_shader *
build_rt_export_const_coord(void)
{
   nir_builder b = cs_builder("cs_rt_export_const_coord");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R8G8B8A8_UNORM, false);
   nir_def *coord = nir_pad_vector_imm_int(&b, nir_imm_ivec2(&b, 0, 0), 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: X/Y constant; only Z/W carry gid. The RT-export predicate requires
 * gid participation in X and Y of the store coordinate. */
static nir_shader *
build_rt_export_gid_zw_only(void)
{
   nir_builder b = cs_builder("cs_rt_export_gid_zw");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R8G8B8A8_UNORM, false);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *gid_xy = nir_trim_vector(&b, gid, 2);
   /* image_*_store coordinates have four components in NIR.  Move the padded
    * zero lanes into X/Y so only Z/W retain gid participation. */
   nir_def *padded_gid = nir_pad_vector_imm_int(&b, gid_xy, 0, 4);
   const unsigned zw_only[4] = { 2, 3, 0, 1 };
   nir_def *coord = nir_swizzle(&b, padded_gid, zw_only, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: array of images. */
static nir_shader *
build_rt_export_array_image(void)
{
   nir_builder b = cs_builder("cs_rt_export_array");
   nir_variable *img =
      cs_image2d_var(&b, "out_imgs", 0, PIPE_FORMAT_R8G8B8A8_UNORM, true);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *coord2 = nir_trim_vector(&b, gid, 2);
   nir_def *coord = nir_pad_vector_imm_int(&b, coord2, 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_deref_instr *base = nir_build_deref_var(&b, img);
   nir_deref_instr *elem =
      nir_build_deref_array(&b, base, nir_imm_int(&b, 0));
   nir_image_deref_store(&b, &elem->def, coord, zero, val, zero,
                         .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: wrong format. */
static nir_shader *
build_rt_export_wrong_format(void)
{
   nir_builder b = cs_builder("cs_rt_export_fmt");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R32G32B32A32_FLOAT, false);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *coord2 = nir_trim_vector(&b, gid, 2);
   /* image_*_store coordinate is always 4 components in NIR. */
   nir_def *coord = nir_pad_vector_imm_int(&b, coord2, 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R32G32B32A32_FLOAT,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: nonzero LOD. */
static nir_shader *
build_rt_export_nonzero_lod(void)
{
   nir_builder b = cs_builder("cs_rt_export_lod");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R8G8B8A8_UNORM, false);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *coord2 = nir_trim_vector(&b, gid, 2);
   nir_def *coord = nir_pad_vector_imm_int(&b, coord2, 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *lod = nir_imm_int(&b, 1);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, lod, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   return b.shader;
}

/* Negative: second competing store_ssbo. */
static nir_shader *
build_rt_export_competing_ssbo(void)
{
   nir_builder b = cs_builder("cs_rt_export_ssbo");
   nir_variable *img =
      cs_image2d_var(&b, "out_img", 0, PIPE_FORMAT_R8G8B8A8_UNORM, false);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *coord2 = nir_trim_vector(&b, gid, 2);
   /* image_*_store coordinate is always 4 components in NIR. */
   nir_def *coord = nir_pad_vector_imm_int(&b, coord2, 0, 4);
   nir_def *val = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, img)->def, coord, zero,
                         val, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                         .image_array = false,
                         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                         .src_type = nir_type_float32);
   nir_store_ssbo(&b, nir_imm_int(&b, 1), nir_imm_int(&b, 1), zero,
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

static void
case_rt_export_metadata(void)
{
   printf("rt-export detector\n");
   /* glsl_array_type for the array-of-images negative needs the type
    * singleton live for the duration of the builders. */
   glsl_type_singleton_init_or_ref();
   {
      nir_shader *s = build_rt_export_ok();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(p.is_rt_exportable, "gid.xy RGBA8 image store admits");
      CHECK(p.image_binding_valid && p.image_binding == 0,
            "binding 0 populated");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_const_coord();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "const coord rejects");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_gid_zw_only();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "gid only in Z/W rejects");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_array_image();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "array-of-images rejects");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_wrong_format();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "non-RGBA8 format rejects");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_nonzero_lod();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "nonzero LOD rejects");
      ralloc_free(s);
   }
   {
      nir_shader *s = build_rt_export_competing_ssbo();
      prepare_detect_shader(s);
      struct r300_image_store_rt_export_pattern p;
      r300_nir_detect_image_store_rt_export(s, &p);
      CHECK(!p.is_rt_exportable, "competing store_ssbo rejects");
      ralloc_free(s);
   }
   glsl_type_singleton_decref();
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
   case_verdict(build_global_scatter(), false, R300_COMPUTE_REJECT_ARBITRARY_SCATTER,
                "global scatter rejects");
   case_verdict(build_global_scatter_2x32(), false,
                R300_COMPUTE_REJECT_ARBITRARY_SCATTER,
                "2x32 global scatter rejects");
   case_verdict(build_dynamic_store_binding(), false,
                R300_COMPUTE_REJECT_RW_STORAGE,
                "dynamic store_ssbo binding rejects");
   case_verdict(build_data_dependent_store_offset(), false,
                R300_COMPUTE_REJECT_RW_STORAGE,
                "data-dependent store_ssbo offset rejects");
   case_verdict(build_lowered_shared_load(), false,
                R300_COMPUTE_REJECT_SHARED_MEMORY,
                "lowered shared load rejects");
   case_verdict(build_lowered_shared_store(), false,
                R300_COMPUTE_REJECT_SHARED_MEMORY,
                "lowered shared store rejects");
   case_verdict(build_fp64(), false, R300_COMPUTE_REJECT_FP64,
                "fp64 arithmetic rejects");
   case_verdict(build_fp64_operand_conversion(), false, R300_COMPUTE_REJECT_FP64,
                "fp64 source operand rejects");
   case_identity_metadata();
   case_binary_metadata();
   case_binary_map_isub_ba_operand_order();
   case_identity_binary_map_offset_gate();
   case_qfmul_metadata();
   case_unary_metadata();
   case_unary_push_metadata();
   case_unary_transcendental_metadata();
   case_binary_transcendental_metadata();
   case_bitwise_logicop_metadata();
   case_shift_logical_metadata();
   case_shift_variable_metadata();
   case_multitap_metadata();
   case_qmul_metadata();
   case_qrotate_metadata();
   case_qconj_metadata();
   case_qnorm_metadata();
   case_qnormalize_metadata();
   case_qfmadd_metadata();
   case_qfmsub_metadata();
   case_qfmmul_metadata();
   case_dp4_metadata();
   case_omul_metadata();
   case_octonion_algebra_metadata();
   case_const_fill_metadata();
   case_constfill_regression();
   case_index_consumption();
   case_compute_global_id_system_value_lowering();
   case_affine_iota();
   case_q16_16_add();
   case_multilimb_mul();
   case_log4_pool();
   case_cas();
   case_rt_export_metadata();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
