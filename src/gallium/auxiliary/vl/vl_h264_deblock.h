/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_deblock_h
#define vl_h264_deblock_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct nir_shader;
struct nir_shader_compiler_options;

/* H.264 in-loop luma deblocking, normal filter, boundary strength 1..3 (ITU-T
 * H.264 sec 8.7.2.3) as an FP24 fragment program for the R300-class back half.
 * The filter reads the six edge samples p2, p1, p0, q0, q1, q2 across a block
 * edge and produces the four possibly-modified samples p1, p0, q0, q1:
 *
 *   filter_on = |p0-q0| < alpha && |p1-p0| < beta && |q1-q0| < beta
 *   tc        = tc0 + (|p2-p0|<beta) + (|q2-q0|<beta)
 *   delta     = clip3(-tc, tc, (((q0-p0)<<2) + (p1-q1) + 4) >> 3)
 *   p0' = clip1(p0 + delta);  q0' = clip1(q0 - delta)
 *   p1' = p1 + clip3(-tc0, tc0, (p2 + ((p0+q0+1)>>1) - (p1<<1)) >> 1)  if |p2-p0|<beta
 *   q1' = q1 + clip3(-tc0, tc0, (q2 + ((p0+q0+1)>>1) - (q1<<1)) >> 1)  if |q2-q0|<beta
 *
 * applied only where filter_on holds.  All gates are predicated selects and all
 * clips are saturating min/max, so the program is one straight-line pass; the
 * largest intermediate |((q0-p0)<<2) + (p1-q1) + 4| <= 1279 stays far under the
 * FP24 integer-exact ceiling 2^17, so the shifts (<<2, <<1 exact, >>3, >>1 via
 * FRC-floor) are bit-exact.
 *
 * Contract this fragment program assumes from the caller's draw state:
 *   - the picture is sampler binding 0, NEAREST + CLAMP_TO_EDGE;
 *   - a varying at VARYING_SLOT_VAR0 carries the p0 sample position in .xy and
 *     the edge-normal texel step in .zw, so the program reads p2..q2 at
 *     base + (k-2)*step;
 *   - a varying at VARYING_SLOT_VAR1 carries alpha, beta, tc0 in .xyz.
 *
 * The four modified samples are written to the RGBA output (p1', p0', q0', q1');
 * mapping them back to their pixel positions is the caller's edge pass.
 */
void *vl_h264_deblock_create_luma_fs(struct pipe_context *pipe);

/* In-place apply kernels: each fragment of a four-sample-wide edge strip writes
 * its own filtered sample (p1', p0', q0', q1' by its strip position), so a strip
 * draw filters an edge segment directly into the picture without a separate
 * scatter.  The strip's [0,1) local coordinate rides in VAR1.x with alpha, beta,
 * tc0 in .yzw; the six edge neighbours are read at offsets relative to the
 * fragment's own position, exact at any plane width.  The vertical kernel walks
 * the edge normal in x, the horizontal kernel in y. */
void *vl_h264_deblock_create_apply_v_fs(struct pipe_context *pipe);
void *vl_h264_deblock_create_apply_h_fs(struct pipe_context *pipe);

/* The same kernels as raw NIR for the r300 compile-budget gate. */
struct nir_shader *
vl_h264_deblock_luma_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_deblock_apply_v_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_deblock_apply_h_nir(const struct nir_shader_compiler_options *options);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_deblock_h */
