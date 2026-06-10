/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_dp4_fs_nir.h"

#include "compiler/nir/nir_builder.h"

/* Pure-NIR DP4 fragment program (no ureg/TGSI -- r300g's nir_to_rc consumes the
 * NIR directly).  Samples in_a, in_b at the fullscreen texcoord and writes
 * dot(a,b) to the RB3D color export; the dot lowers to the US DP4 instruction,
 * byte-exact for <=7-bit (quantized) operands (4*127^2=64516<2^17, hardware-
 * confirmed).  Built with the same nir_builder pattern as the vl_nir FS helpers
 * (compiler/nir/nir_builder.h); gather_info + assign_io_var_locations run here
 * because nir_to_rc keys interpolators off driver_location. */
nir_shader *
r300vk_build_dp4_fs_nir(const nir_shader_compiler_options *opts,
                        unsigned components)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_dp4");

   nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   /* The varying is a vec4, but a 2D sampler takes a 2-component (s,t)
    * coordinate.  nir_tex asserts coord_components equals the sampler
    * dimension's coordinate count, so trim the load to xy before building
    * the texture instruction. */
   nir_def *coord = nir_trim_vector(&b, nir_load_var(&b, in_tc), 2);

   nir_def *tex[2];
   for (unsigned s = 0; s < 2; s++) {
      nir_variable *samp = nir_variable_create(
         b.shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         "samp");
      samp->data.binding = s;
      nir_deref_instr *d = nir_build_deref_var(&b, samp);
      tex[s] = nir_tex(&b, coord, .texture_deref = d, .sampler_deref = d);
   }

   unsigned c = components ? components : 4;
   /* tex[s] is a vec4 sample; trim to the kernel's dot width (2/3/4).
    * nir_trim_vector returns the def unchanged when c == 4, so the
    * full-width DP4 case adds no extract.  Trimming via the helper (rather
    * than nir_channels(BITFIELD_MASK(c))) avoids feeding the macro's ~0u
    * branch into nir_component_mask_t, which would be a u16 truncation. */
   nir_def *a  = nir_trim_vector(&b, tex[0], c);
   nir_def *bb = nir_trim_vector(&b, tex[1], c);
   nir_def *dot = nir_fdot(&b, a, bb);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   /* R300 has no FP32 render target (hardware-confirmed: an FP32 color FBO is
    * incomplete), so the scalar dot cannot be written as an IEEE-754 float.
    * Carry it as a 3-byte little-endian integer in RGBA8: r=dot%256,
    * g=(dot/256)%256, b=(dot/65536)%256.  The dot is an integer <= 2^17 for
    * <=7-bit (quantized) operands and stays exact through this FP24 encode
    * (every intermediate <= 2^24).  The dispatch-replay's UNORM8 RT round-trips
    * each byte and copies them to the kernel's uint output SSBO.  This is the
    * same encode the surfaceless-EGL dp4 probe proved 6/6 byte-exact. */
   nir_def *fl256 = nir_ffloor(&b, nir_fmul_imm(&b, dot, 1.0 / 256.0));
   nir_def *enc_r = nir_fsub(&b, dot, nir_fmul_imm(&b, fl256, 256.0));
   nir_def *enc_g = nir_fsub(&b, fl256,
      nir_fmul_imm(&b, nir_ffloor(&b, nir_fmul_imm(&b, fl256, 1.0 / 256.0)), 256.0));
   nir_def *flh = nir_ffloor(&b, nir_fmul_imm(&b, dot, 1.0 / 65536.0));
   nir_def *enc_b = nir_fsub(&b, flh,
      nir_fmul_imm(&b, nir_ffloor(&b, nir_fmul_imm(&b, flh, 1.0 / 256.0)), 256.0));
   nir_def *enc = nir_vec4(&b,
      nir_fmul_imm(&b, enc_r, 1.0 / 255.0), nir_fmul_imm(&b, enc_g, 1.0 / 255.0),
      nir_fmul_imm(&b, enc_b, 1.0 / 255.0), nir_imm_float(&b, 0.0));
   nir_store_var(&b, out, enc, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* The quaternion Hamilton product q1*q2 as four sign-permuted DP4s -- the
 * Cayley-Dickson dim-4 multiply.  Each output lane is one DP4 of q1 against a
 * sign-permuted swizzle of q2; the four permutations are the Hamilton-matrix
 * rows in (w,x,y,z) layout, verified against the catalog self-check
 * (1,2,3,4)*(5,6,7,8) = (-60,12,30,24).  The negated lanes are native fneg in a
 * vec4 constructor, which nir_to_rc lowers correctly since the srcmod-fold
 * register-dest fix (before it, a negated variable lane read back zero).
 * Shared by QMUL (one product) and QROTATE (two products = the sandwich). */
static nir_def *
hamilton_product(nir_builder *b, nir_def *q1, nir_def *q2)
{
   nir_def *w2 = nir_channel(b, q2, 0), *x2 = nir_channel(b, q2, 1);
   nir_def *y2 = nir_channel(b, q2, 2), *z2 = nir_channel(b, q2, 3);

   nir_def *perm_w = nir_vec4(b, w2, nir_fneg(b, x2), nir_fneg(b, y2),
                              nir_fneg(b, z2));
   nir_def *perm_x = nir_vec4(b, x2, w2, z2, nir_fneg(b, y2));
   nir_def *perm_y = nir_vec4(b, y2, nir_fneg(b, z2), w2, x2);
   nir_def *perm_z = nir_vec4(b, z2, y2, nir_fneg(b, x2), w2);

   return nir_vec4(b, nir_fdot(b, q1, perm_w), nir_fdot(b, q1, perm_x),
                   nir_fdot(b, q1, perm_y), nir_fdot(b, q1, perm_z));
}

/* QMUL_HAMILTON: sample q1, q2 at the fullscreen texcoord and write q1*q2 to the
 * FP16 color export -- one Hamilton product, four DP4s. */
nir_shader *
r300vk_build_qmul_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_qmul");

   nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   /* 2D sampler takes a 2-component coordinate; nir_tex asserts coord_components
    * matches the sampler dimension, so trim the vec4 varying to xy. */
   nir_def *coord = nir_trim_vector(&b, nir_load_var(&b, in_tc), 2);

   nir_def *q[2];
   for (unsigned s = 0; s < 2; s++) {
      nir_variable *samp = nir_variable_create(
         b.shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         "samp");
      samp->data.binding = s;
      nir_deref_instr *d = nir_build_deref_var(&b, samp);
      q[s] = nir_tex(&b, coord, .texture_deref = d, .sampler_deref = d);
   }

   nir_def *prod = hamilton_product(&b, q[0], q[1]);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   /* The product is a four-component quaternion; write it straight to the color
    * export.  The substrate renders quaternion results to an FP16 (RGBA16F)
    * target -- R300 has no FP32 RT, but FP16 carries each lane for the admitted
    * operand range -- so no per-byte encode is needed here, unlike the scalar
    * DP4 path. */
   nir_store_var(&b, out, prod, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* QROTATE_SANDWICH: rotate the vec3 v by the unit quaternion q as
 * q * embed(v) * conj(q) -- two Hamilton products, eight DP4s.  Samples q
 * (binding 0) and v (binding 1); embed(v) = (0, vx, vy, vz) and conj(q) =
 * (qw, -qx, -qy, -qz) in (w,x,y,z) layout.  The product's vector lanes carry
 * the rotated v (the scalar lane is ~0 for a unit q).  Writes the result to the
 * FP16 color export, like QMUL. */
nir_shader *
r300vk_build_qrotate_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_qrotate");

   nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   nir_def *coord = nir_trim_vector(&b, nir_load_var(&b, in_tc), 2);

   nir_def *tex[2];
   for (unsigned s = 0; s < 2; s++) {
      nir_variable *samp = nir_variable_create(
         b.shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         "samp");
      samp->data.binding = s;
      nir_deref_instr *d = nir_build_deref_var(&b, samp);
      tex[s] = nir_tex(&b, coord, .texture_deref = d, .sampler_deref = d);
   }
   nir_def *q = tex[0], *v = tex[1];

   nir_def *embed_v = nir_vec4(&b, nir_imm_float(&b, 0.0f),
                               nir_channel(&b, v, 0), nir_channel(&b, v, 1),
                               nir_channel(&b, v, 2));
   nir_def *conj_q = nir_vec4(&b, nir_channel(&b, q, 0),
                              nir_fneg(&b, nir_channel(&b, q, 1)),
                              nir_fneg(&b, nir_channel(&b, q, 2)),
                              nir_fneg(&b, nir_channel(&b, q, 3)));

   /* q * embed(v) * conj(q). */
   nir_def *t = hamilton_product(&b, q, embed_v);
   nir_def *rotated = hamilton_product(&b, t, conj_q);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, rotated, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* Sample one float quaternion at the fullscreen texcoord from sampler binding 0.
 * QCONJ and QNORM both read a single input; this folds the shared varying +
 * sampler setup so each builder only states its arithmetic. */
static nir_def *
sample_single_quaternion(nir_builder *b)
{
   nir_variable *in_tc = nir_variable_create(b->shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   nir_def *coord = nir_trim_vector(b, nir_load_var(b, in_tc), 2);

   nir_variable *samp = nir_variable_create(
      b->shader, nir_var_uniform,
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
      "samp");
   samp->data.binding = 0;
   nir_deref_instr *d = nir_build_deref_var(b, samp);
   return nir_tex(b, coord, .texture_deref = d, .sampler_deref = d);
}

/* QCONJ: write the quaternion conjugate (a.x, -a.y, -a.z, -a.w) of the single
 * sampled input to the FP16 color export -- a sign flip on the vector lanes,
 * zero DP4.  The negated lanes are native fneg in the vec4 constructor (correct
 * since the nir_to_rc srcmod-fold register-dest fix). */
nir_shader *
r300vk_build_qconj_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_qconj");

   nir_def *a = sample_single_quaternion(&b);
   nir_def *conj = nir_vec4(&b, nir_channel(&b, a, 0),
                            nir_fneg(&b, nir_channel(&b, a, 1)),
                            nir_fneg(&b, nir_channel(&b, a, 2)),
                            nir_fneg(&b, nir_channel(&b, a, 3)));

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, conj, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* QNORM: write the squared norm dot(a, a) broadcast across all four lanes to the
 * FP16 color export -- one DP4.  The substrate reads lane 0; broadcasting keeps
 * the vec4 readback path (the kernel's output SSBO is vec4 FP32). */
nir_shader *
r300vk_build_qnorm_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_qnorm");

   nir_def *a = sample_single_quaternion(&b);
   nir_def *n = nir_fdot(&b, a, a);
   nir_def *bn = nir_vec4(&b, n, n, n, n);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, bn, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* The quaternion conjugate (w, -x, -y, -z) -- the Cayley-Dickson involution that
 * the octonion product mixes into two of its four Hamilton products. */
static nir_def *
quat_conj(nir_builder *b, nir_def *q)
{
   return nir_vec4(b, nir_channel(b, q, 0), nir_fneg(b, nir_channel(b, q, 1)),
                   nir_fneg(b, nir_channel(b, q, 2)), nir_fneg(b, nir_channel(b, q, 3)));
}

/* Sample the four quaternion halves of two octonions x=(a,b) and y=(c,d) at the
 * fullscreen texcoord from sampler bindings 0..3 (a, b, c, d).  Both octonion-
 * product passes read all four, so fold the shared varying + sampler setup. */
static void
sample_octonion_halves(nir_builder *b, nir_def **a, nir_def **bb,
                       nir_def **c, nir_def **d)
{
   nir_variable *in_tc = nir_variable_create(b->shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   nir_def *coord = nir_trim_vector(b, nir_load_var(b, in_tc), 2);

   nir_def *t[4];
   for (unsigned s = 0; s < 4; s++) {
      nir_variable *samp = nir_variable_create(
         b->shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         "samp");
      samp->data.binding = s;
      nir_deref_instr *dref = nir_build_deref_var(b, samp);
      t[s] = nir_tex(b, coord, .texture_deref = dref, .sampler_deref = dref);
   }
   *a = t[0]; *bb = t[1]; *c = t[2]; *d = t[3];
}

/* OMUL lower half: the first quaternion of the octonion product (a,b)*(c,d) is
 * a*c - conj(d)*b (Cayley-Dickson doubling).  Two Hamilton products = eight DP4s,
 * differenced, to the FP16 color export.  Samples a,b,c,d at bindings 0..3. */
nir_shader *
r300vk_build_omul_lo_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_omul_lo");

   nir_def *a, *bb, *c, *d;
   sample_octonion_halves(&b, &a, &bb, &c, &d);
   nir_def *ac  = hamilton_product(&b, a, c);
   nir_def *dcb = hamilton_product(&b, quat_conj(&b, d), bb);
   nir_def *lo  = nir_fsub(&b, ac, dcb);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, lo, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* OMUL upper half: the second quaternion of (a,b)*(c,d) is d*a + b*conj(c).  Two
 * Hamilton products = eight DP4s, summed, to the FP16 color export.  Samples
 * a,b,c,d at bindings 0..3, like the lower-half pass. */
nir_shader *
r300vk_build_omul_hi_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_omul_hi");

   nir_def *a, *bb, *c, *d;
   sample_octonion_halves(&b, &a, &bb, &c, &d);
   nir_def *da  = hamilton_product(&b, d, a);
   nir_def *bcc = hamilton_product(&b, bb, quat_conj(&b, c));
   nir_def *hi  = nir_fadd(&b, da, bcc);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, hi, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}

/* OMUL both halves in ONE pass via multiple render targets: write the lower half
 * a*c - conj(d)*b to color output 0 (FRAG_RESULT_DATA0) and the upper half
 * d*a + b*conj(c) to color output 1 (FRAG_RESULT_DATA1).  Sixteen DP4s -- all
 * four Hamilton products -- in a single draw.  The MRT dispatch route uses this
 * when the screen supports two simultaneous FP16 render targets; otherwise the
 * substrate falls back to the two single-output passes (omul_lo + omul_hi). */
nir_shader *
r300vk_build_omul_mrt_fs_nir(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_omul_mrt");

   nir_def *a, *bb, *c, *d;
   sample_octonion_halves(&b, &a, &bb, &c, &d);
   nir_def *lo = nir_fsub(&b, hamilton_product(&b, a, c),
                          hamilton_product(&b, quat_conj(&b, d), bb));
   nir_def *hi = nir_fadd(&b, hamilton_product(&b, d, a),
                          hamilton_product(&b, bb, quat_conj(&b, c)));

   nir_variable *out0 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "color0");
   out0->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, out0, lo, 0xf);
   nir_variable *out1 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "color1");
   out1->data.location = FRAG_RESULT_DATA1;
   nir_store_var(&b, out1, hi, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}
