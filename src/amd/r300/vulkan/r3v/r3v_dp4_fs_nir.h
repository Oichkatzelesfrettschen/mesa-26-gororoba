/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_DP4_FS_NIR_H
#define R3V_DP4_FS_NIR_H

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the DP4 compute-as-raster fragment program as standalone NIR: sample
 * two 2D-sampler inputs at the fullscreen texcoord, dot the first `components`
 * channels (2, 3, or 4; 0 means 4), truncate the dot as the admitted
 * f2u32(fdot(...)) compute store does, and encode the integer dot into RGBA8
 * as a 4-byte little-endian value.  The caller runs screen->finalize_nir and
 * create_fs_state.
 *
 * Split out of r3v_synthesize_dp4_fs so a build-time test can validate the
 * shader shape without a pipe_context -- in particular that the 2D sampler
 * receives a 2-component coordinate, the invariant nir_build_tex_struct
 * asserts and whose violation aborted every DP4 compute pipeline create on an
 * asserts-enabled build. */
nir_shader *r3v_build_dp4_fs_nir(const nir_shader_compiler_options *opts,
                                    unsigned components);

/* Build the quaternion Hamilton-product fragment program as standalone NIR.
 * Samples two input quaternions (q1 = sampler binding 0, q2 = binding 1) at the
 * fullscreen texcoord and writes q1*q2 to the color export as four DP4s -- the
 * Cayley-Dickson dim-4 multiply, QMUL_HAMILTON in r300_virtual_op_catalog.  In
 * the (w,x,y,z) layout the product is dot(q1, sign-permuted q2) per lane:
 *   w = dot(q1,( w2,-x2,-y2,-z2)),  x = dot(q1,( x2, w2, z2,-y2)),
 *   y = dot(q1,( y2,-z2, w2, x2)),  z = dot(q1,( z2, y2,-x2, w2)).
 * The negated lanes use native fneg in the vec4 constructor, correct since the
 * nir_to_rc srcmod-fold register-dest fix.  The four-component product is
 * written straight to the FP16 color export -- the substrate's quaternion
 * output format -- not the DP4 path's 3-byte RGBA8 scalar encode. */
nir_shader *r3v_build_qmul_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion-division fragment program (QDIV) as standalone NIR.  Samples
 * the dividend a (binding 0) and divisor b (binding 1) and writes a*inv(b),
 * inv(b) = conj(b)*rcp(dot(b,b)) -- one Hamilton product (four DP4s) over the scaled
 * conjugate, one pass (a quaternion divide stays under the 64-ALU limit, unlike the
 * octonion ODIV which splits across two passes) -- to the FP16 color export. */
nir_shader *r3v_build_qdiv_fs_nir(const nir_shader_compiler_options *opts);

/* MAT4VEC's fragment program is synthesized in ureg/TGSI by
 * r3v_synthesize_mat4vec_fs (r3v_pipeline.c), not built here: the broadcast
 * 4x4 lives in the constant file (CONST[0..3]) rather than a texture, so the
 * program is 1 TEX (the per-element vertex) + 4 DP4 (one const row each). */

/* Build the quaternion-rotation fragment program (QROTATE_SANDWICH) as standalone
 * NIR.  Samples a unit quaternion q (binding 0) and a vector v (binding 1) and
 * writes q * embed(v) * conj(q) -- two Hamilton products, eight DP4s -- to the
 * FP16 color export; the result's vector lanes are the rotated v. */
nir_shader *r3v_build_qrotate_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion-conjugate fragment program (QCONJ) as standalone NIR.
 * Samples one input quaternion (binding 0) and writes (a.x, -a.y, -a.z, -a.w) to
 * the FP16 color export -- a sign flip on the three vector lanes, zero DP4. */
nir_shader *r3v_build_qconj_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion squared-norm fragment program (QNORM) as standalone NIR.
 * Samples one input quaternion (binding 0) and writes vec4(dot(a, a)) -- the
 * squared norm broadcast across four lanes -- to the FP16 color export, one
 * DP4.  The kernel reads lane 0; the broadcast keeps the vec4 readback path. */
nir_shader *r3v_build_qnorm_fs_nir(const nir_shader_compiler_options *opts);

/* QNORMALIZE FS: out = a * rsqrt(dot(a,a)) = a / |a|.  One DP4 + US RSQ + scale. */
nir_shader *r3v_build_qnormalize_fs_nir(const nir_shader_compiler_options *opts);

/* Build the two octonion-product (OMUL) passes as standalone NIR.  An octonion
 * (a,b) is two quaternions; the product (a,b)*(c,d) = (a*c - conj(d)*b,
 * d*a + b*conj(c)) is four Hamilton products = sixteen DP4s, the Cayley-Dickson
 * rung above the quaternion.  Each pass samples the four quaternion halves
 * a,b,c,d at bindings 0..3 and writes one output quaternion to the FP16 color
 * export: the _lo pass the first quaternion (a*c - conj(d)*b), the _hi pass the
 * second (d*a + b*conj(c)).  Eight DP4s per pass; the substrate runs both to
 * fill the eight-wide octonion result. */
nir_shader *r3v_build_omul_lo_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_omul_hi_fs_nir(const nir_shader_compiler_options *opts);

/* Build the single-pass multiple-render-target octonion product: both halves in
 * one FS, the lower half to color output 0 (FRAG_RESULT_DATA0) and the upper to
 * color output 1 (FRAG_RESULT_DATA1) -- sixteen DP4s in one draw.  The dispatch's
 * MRT route uses this when the screen supports two simultaneous FP16 render
 * targets; otherwise it falls back to the two single-output passes. */
nir_shader *r3v_build_omul_mrt_fs_nir(const nir_shader_compiler_options *opts);

/* Octonion elementwise-algebra fragment programs.
 * ONORM: |(a,b)|^2 = dot(a,a)+dot(b,b) broadcast (a stage 0, b stage 1), one
 *   color output; two DP4s.
 * OCONJ (MRT): conj(a)=(a.x,-a.y,-a.z,-a.w) to output 0 and -b to output 1
 *   (a stage 0, b stage 1).
 * OADD/OSUB (MRT): the lower half a (+|-) c to output 0 and the upper b (+|-) d
 *   to output 1; the dispatch binds stage0=a, stage1=c, stage2=b, stage3=d so
 *   each half reads a contiguous sampler pair.  is_sub picks the operator. */
nir_shader *r3v_build_onorm_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_oconj_mrt_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_oaddsub_mrt_fs_nir(const nir_shader_compiler_options *opts,
                                            bool is_sub);

/* ODIV (octonion right division) fragment programs: out = x * inv(y),
 * inv(y) = conj(y)/|y|^2.  Samples x at stages 0,1 and y at stages 2,3, forms the
 * inverse from the scalar reciprocal rcp(|y|^2), and emits one half of the
 * eight-wide product x*inv(y) per pass.  Division runs as two single-output passes
 * because the combined MRT form exceeds the 64-ALU R300 fragment limit. */
nir_shader *r3v_build_odiv_lo_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_odiv_hi_fs_nir(const nir_shader_compiler_options *opts);

/* ODIV_L (octonion left division) half fragment programs: out = inv(y)*x.  Same
 * inverse + two-pass structure as ODIV, with the OMUL operands swapped. */
nir_shader *r3v_build_odiv_l_lo_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_odiv_l_hi_fs_nir(const nir_shader_compiler_options *opts);

/* OTRANS (octonion sandwich x*v*conj(x)) second-pass half fragment programs:
 * out = t*conj(x) where t = x*v (pass 1 uses the OMUL half-shaders).  Sample t at
 * stages 0,1 and x at stages 2,3; conj(x) is formed inline. */
nir_shader *r3v_build_otrans_p2_lo_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_otrans_p2_hi_fs_nir(const nir_shader_compiler_options *opts);

/* QFM fused fragment programs (three inputs sampled at stages 0,1,2):
 * QFMADD out = a*b + c and QFMSUB out = a*b - c share one builder;
 * QFMMUL out = a*b*c = (a*b)*c (one pass). */
nir_shader *r3v_build_qfmadd_fs_nir(const nir_shader_compiler_options *opts,
                                    bool is_sub);
nir_shader *r3v_build_qfmmul_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_ieee16_classify_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r3v_build_ieee16_mul_fs_nir(const nir_shader_compiler_options *opts);

#ifdef __cplusplus
}
#endif

#endif /* R3V_DP4_FS_NIR_H */
