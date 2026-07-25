/*
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_chroma_h
#define vl_h264_chroma_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct nir_shader;
struct nir_shader_compiler_options;

/* H.264 chroma eighth-pel motion compensation (ITU-T H.264 sec 8.4.2.2.2) as an
 * FP24 fragment program for the R300-class back half.  A chroma sample is the
 * bilinear blend of the 2x2 integer-sample neighborhood with eighth-pel weights:
 *
 *   ((8-xF)(8-yF) A + xF(8-yF) B + (8-xF)yF C + xF*yF D + 32) >> 6
 *
 * where A, B, C, D are the top-left, top-right, bottom-left, and bottom-right
 * integer samples and xF, yF are the eighth-pel fraction in 0..7.  The four
 * weights sum to 64, so the accumulator peaks at 64*255 = 16320 -- well under
 * the FP24 integer-exact ceiling 2^17 -- and the result lands in [0, 255]
 * without a clip.  This is the hardware bilinear done explicitly on the FP24
 * ALU rather than by the texture filter, which cannot reproduce the spec's
 * fixed-point eighth-pel weights and +32 >> 6 rounding bit-exactly.
 *
 * Contract this fragment program assumes from the caller's draw state:
 *   - the reference chroma plane is sampler binding 0, NEAREST + CLAMP_TO_EDGE
 *     (the H.264 reference picture is edge-extended; R300 has no texelFetch);
 *   - a varying at VARYING_SLOT_VAR0 carries the sample position in .xy
 *     (normalized [0,1]) and the reference texel step in .zw (1/width, 1/height),
 *     so the program reads A at the position and B, C, D one texel right, down,
 *     and diagonal;
 *   - a varying at VARYING_SLOT_VAR1 carries the four integer weights in xyzw
 *     (the caller derives them from the motion vector eighth-pel fraction once
 *     per block).
 */
void *vl_h264_chroma_create_bilinear_fs(struct pipe_context *pipe);

/* The same kernel as raw NIR for the r300 compile-budget gate. */
struct nir_shader *
vl_h264_chroma_bilinear_nir(const struct nir_shader_compiler_options *options);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_chroma_h */
