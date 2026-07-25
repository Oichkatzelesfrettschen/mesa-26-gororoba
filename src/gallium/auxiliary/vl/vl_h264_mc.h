/*
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_mc_h
#define vl_h264_mc_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct nir_shader;
struct nir_shader_compiler_options;

/* H.264 luma half-pel motion compensation (ITU-T H.264 sec 8.4.2.2.1) as FP24
 * fragment programs for the R300-class back half.  A half-pel sample is the
 * six-tap FIR (1, -5, 20, 20, -5, 1) over six integer luma samples along one
 * axis, normalized (sum + 16) >> 5 and clipped to [0, 255].  This is the
 * single-axis case (positions b and h); the 2D diagonal (position j) runs the
 * filter un-normalized on the first axis and overflows the FP24 integer-exact
 * ceiling 2^17 at its achievable peak, so it stays on the CPU.
 *
 * Contract these fragment programs assume from the caller's draw state:
 *   - the reference luma plane is sampler binding 0, filtered NEAREST with
 *     CLAMP_TO_EDGE (the H.264 reference picture is edge-extended; R300 has no
 *     texelFetch, nir_to_rc rejects nir_texop_txf);
 *   - a varying at VARYING_SLOT_VAR0 carries the sample position in .xy
 *     (normalized [0,1]) and the reference texel step in .zw (1/width, 1/height),
 *     so the program reads the six taps at the fixed integer-sample centers
 *     around the position without knowing the plane size.
 *
 * The horizontal program (position b) walks .x by the .z step; the vertical
 * program (position h) walks .y by the .w step. */
void *vl_h264_mc_create_halfpel_h_fs(struct pipe_context *pipe);
void *vl_h264_mc_create_halfpel_v_fs(struct pipe_context *pipe);

/* Quarter-pel luma motion compensation: the eight FP24-feasible sub-pel
 * positions are a two-tap average (a + b + 1) >> 1 of two adjacent samples, both
 * computed inline from the reference so their six-taps clamp at the picture edge.
 * The axis kernel (positions a, c, d, n) averages a half-pel along one axis with
 * an integer; the diagonal kernel (positions e, g, p, r) averages a half-pel-
 * horizontal sample with a half-pel-vertical sample.  Both take the position and
 * texel steps in VARYING_SLOT_VAR0 and the per-position offsets in
 * VARYING_SLOT_VAR1: the axis kernel reads the tap direction in .xy and the
 * integer offset in .zw; the diagonal kernel reads the horizontal half-pel offset
 * in .xy and the vertical in .zw.  The five positions that need the 2D half-pel-
 * diagonal (j and its neighbours) overflow FP24 and have no kernel here. */
void *vl_h264_mc_create_qpel_axis_fs(struct pipe_context *pipe);
void *vl_h264_mc_create_qpel_diag_fs(struct pipe_context *pipe);

/* The same kernels as raw NIR for the r300 compile-budget gate. */
struct nir_shader *
vl_h264_mc_halfpel_h_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_mc_halfpel_v_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_mc_qpel_axis_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_mc_qpel_diag_nir(const struct nir_shader_compiler_options *options);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_mc_h */
