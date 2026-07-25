/*
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_idct_h
#define vl_h264_idct_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct nir_shader;
struct nir_shader_compiler_options;

/* H.264 4x4 inverse integer transform (ITU-T H.264 sec 8.5.12.2) as a two-pass
 * separable FP24 fragment program for the R300-class back half.  The transform
 * is the H.264 inverse butterfly -- adds, subtracts, and arithmetic right shifts
 * -- not the MPEG-2 DCT matrix multiply vl_idct.c emits, so it carries its own
 * shaders.  Each pass is one separable 1-D butterfly: the row pass runs ~17 ALU
 * and the column-plus-normalize pass ~23, both well inside the R300 non-HB
 * fragment limits (64 ALU, 32 TEX, 32 temp), where a single-pass whole-block
 * shader would run all four row butterflies per fragment and crowd the ceiling.
 *
 * Contract these fragment programs assume from the caller's draw state:
 *   - input is a per-texel R32_FLOAT 4x4 texture, texel (col,row) = the
 *     dequantized coefficient in canonical pixel-natural order coeff[row][col]
 *     (the provider has already transposed the entropy decoder's scan order);
 *   - sampler binding 0 filters NEAREST, normalized coordinates (R300 has no
 *     texelFetch: nir_to_rc rejects nir_texop_txf);
 *   - a varying at VARYING_SLOT_VAR0 carries the block's [0,1] texcoord, so the
 *     row pass reads along the fixed texcoord.y and selects its output column
 *     from texcoord.x, and the column pass reads along texcoord.x and selects
 *     its output row from texcoord.y.  The cross-axis reads use the four fixed
 *     texel centers (k + 0.5)/4.
 *
 * Run the row pass into a 4x4 R32_FLOAT intermediate, then the column pass into
 * the 4x4 R32_FLOAT residual.  The two write/read addressings absorb the spec's
 * row-then-column-then-transpose so no explicit transpose op is emitted:
 * residual[i][c] = ((column_butterfly(row_butterfly over column c)[i]) + 32)>>6.
 */
void *vl_h264_idct_create_row_fs(struct pipe_context *pipe);
void *vl_h264_idct_create_col_fs(struct pipe_context *pipe);

/* The same two kernels as raw NIR for the r300 compile-budget gate. */
struct nir_shader *
vl_h264_idct_row_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_idct_col_nir(const struct nir_shader_compiler_options *options);

/* Whole-plane variants of the two passes: the same separable butterfly run over
 * a plane of tiled 4x4 blocks in a single draw, so a 16x16 luma macroblock (four
 * by four blocks) or an 8x8 chroma plane is transformed without one draw per
 * block.  The coefficient plane is per-texel R32_FLOAT sized 4*blocks_x by
 * 4*blocks_y, block (bx,by) occupying texels [4*bx, 4*bx+4) x [4*by, 4*by+4) in
 * canonical pixel-natural order; the caller scatters the contract's per-block
 * coefficients into that layout.  Sampler binding 0 filters NEAREST with
 * normalized coordinates; a varying at VARYING_SLOT_VAR0 carries (u, v, 1/W, 1/H)
 * and VARYING_SLOT_VAR1 carries (W, H, _, _), the plane width and height in
 * texels (powers of two), from which the fragment derives its block and in-block
 * position.  Run the row pass into a same-sized R32_FLOAT intermediate plane,
 * then the column pass into the residual plane. */
void *vl_h264_idct_create_plane_row_fs(struct pipe_context *pipe);
void *vl_h264_idct_create_plane_col_fs(struct pipe_context *pipe);

struct nir_shader *
vl_h264_idct_plane_row_nir(const struct nir_shader_compiler_options *options);
struct nir_shader *
vl_h264_idct_plane_col_nir(const struct nir_shader_compiler_options *options);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_idct_h */
