/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_DP4_FS_NIR_H
#define R300VK_DP4_FS_NIR_H

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the DP4 compute-as-raster fragment program as standalone NIR: sample
 * two 2D-sampler inputs at the fullscreen texcoord, dot the first `components`
 * channels (2, 3, or 4; 0 means 4), and encode the integer dot into RGBA8 as a
 * 3-byte little-endian value.  The caller runs screen->finalize_nir and
 * create_fs_state.
 *
 * Split out of r300vk_synthesize_dp4_fs so a build-time test can validate the
 * shader shape without a pipe_context -- in particular that the 2D sampler
 * receives a 2-component coordinate, the invariant nir_build_tex_struct
 * asserts and whose violation aborted every DP4 compute pipeline create on an
 * asserts-enabled build. */
nir_shader *r300vk_build_dp4_fs_nir(const nir_shader_compiler_options *opts,
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
nir_shader *r300vk_build_qmul_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion-rotation fragment program (QROTATE_SANDWICH) as standalone
 * NIR.  Samples a unit quaternion q (binding 0) and a vector v (binding 1) and
 * writes q * embed(v) * conj(q) -- two Hamilton products, eight DP4s -- to the
 * FP16 color export; the result's vector lanes are the rotated v. */
nir_shader *r300vk_build_qrotate_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion-conjugate fragment program (QCONJ) as standalone NIR.
 * Samples one input quaternion (binding 0) and writes (a.x, -a.y, -a.z, -a.w) to
 * the FP16 color export -- a sign flip on the three vector lanes, zero DP4. */
nir_shader *r300vk_build_qconj_fs_nir(const nir_shader_compiler_options *opts);

/* Build the quaternion squared-norm fragment program (QNORM) as standalone NIR.
 * Samples one input quaternion (binding 0) and writes vec4(dot(a, a)) -- the
 * squared norm broadcast across four lanes -- to the FP16 color export, one
 * DP4.  The kernel reads lane 0; the broadcast keeps the vec4 readback path. */
nir_shader *r300vk_build_qnorm_fs_nir(const nir_shader_compiler_options *opts);

/* Build the two octonion-product (OMUL) passes as standalone NIR.  An octonion
 * (a,b) is two quaternions; the product (a,b)*(c,d) = (a*c - conj(d)*b,
 * d*a + b*conj(c)) is four Hamilton products = sixteen DP4s, the Cayley-Dickson
 * rung above the quaternion.  Each pass samples the four quaternion halves
 * a,b,c,d at bindings 0..3 and writes one output quaternion to the FP16 color
 * export: the _lo pass the first quaternion (a*c - conj(d)*b), the _hi pass the
 * second (d*a + b*conj(c)).  Eight DP4s per pass; the substrate runs both to
 * fill the eight-wide octonion result. */
nir_shader *r300vk_build_omul_lo_fs_nir(const nir_shader_compiler_options *opts);
nir_shader *r300vk_build_omul_hi_fs_nir(const nir_shader_compiler_options *opts);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_DP4_FS_NIR_H */
