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
   CHECK(!umap.is_unary_map,
         "scalar unary-map rejected until a scalar carrier exists");
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

/* Constant-fill kernel: out_buffer[gid] = 0x42424242u (no loads). */
static nir_shader *
build_const_fill_u32(void)
{
   nir_builder b = cs_builder("cs_const_fill_u32");
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
   nir_store_ssbo(&b, c, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                  .write_mask = 0xf, .align_mul = 16, .align_offset = 0);
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

   /* Vec4 constant fill: verify detection of vec4 constant. */
   nir_shader *nir4 = build_const_fill_vec4();
   struct r300_compute_const_fill_pattern cf4 = {0};

   prepare_detect_shader(nir4);
   r300_nir_detect_const_fill_pattern(nir4, &cf4);
   CHECK(cf4.is_const_fill, "const-fill vec4 shape detected");
   CHECK(cf4.value_components == 4, "const-fill vec4 records vec4 width");
   CHECK(cf4.output_ssbo_binding_valid, "const-fill vec4 records binding");
   ralloc_free(nir4);

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
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* 0x00000000 fill: zero sentinel exercises the all-zero byte path. */
static nir_shader *
build_const_fill_zero(void)
{
   nir_builder b = cs_builder("cs_const_fill_zero");
   nir_def *c = nir_imm_int(&b, 0x00000000);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* 0xFFFFFFFF fill: saturation ceiling -- all bytes are 0xFF. */
static nir_shader *
build_const_fill_ff(void)
{
   nir_builder b = cs_builder("cs_const_fill_ff");
   nir_def *c = nir_imm_int(&b, 0xFFFFFFFF);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 0x1, .align_mul = 4, .align_offset = 0);
   return b.shader;
}

/* Fill at binding 7: non-default binding, verifies output_ssbo_binding capture. */
static nir_shader *
build_const_fill_binding7(void)
{
   nir_builder b = cs_builder("cs_const_fill_binding7");
   nir_def *c = nir_imm_int(&b, 0x11223344);
   nir_store_ssbo(&b, c, nir_imm_int(&b, 7), nir_imm_int(&b, 0),
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
   case_verdict(build_fp64(), false, R300_COMPUTE_REJECT_FP64,
                "fp64 arithmetic rejects");
   case_identity_metadata();
   case_binary_metadata();
   case_qfmul_metadata();
   case_unary_metadata();
   case_qmul_metadata();
   case_qrotate_metadata();
   case_qconj_metadata();
   case_qnorm_metadata();
   case_omul_metadata();
   case_octonion_algebra_metadata();
   case_const_fill_metadata();
   case_constfill_regression();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
