/*
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"
#include "vl_h264_idct.h"

/* Arithmetic right shift on the shiftless FP24 fragment ALU.  The spec's
 * z >> k is realized as floor(z * 2^-k): scale, take the fractional part with
 * FRC, and subtract it.  FRC floors toward minus infinity, so the result
 * matches a two's-complement arithmetic shift for negative residuals as well.
 * Every back-half intermediate stays under the FP24 integer-exact ceiling 2^17,
 * where z * 2^-k is exact and the floor is bit-exact (validated bit-exact
 * against the integer transform within the QP coefficient envelope). */
static nir_def *
fp24_floor_shift(nir_builder *b, nir_def *value, unsigned shift)
{
   nir_def *scaled = nir_fmul(b, value,
                              nir_imm_float(b, 1.0f / (float)(1u << shift)));
   return nir_fadd(b, scaled, nir_fneg(b, nir_ffract(b, scaled)));
}

/* One H.264 inverse core butterfly over a 4-vector (ITU-T H.264 sec 8.5.12.2):
 *   a = z0 + z2;  b = z0 - z2;  c = (z1 >> 1) - z3;  d = z1 + (z3 >> 1)
 *   out = [a + d, b + c, b - c, a - d].
 * Mirrors _idct4_1d operation for operation; the two half-weight terms are the
 * arithmetic right shifts above. */
static void
idct4_butterfly(nir_builder *b, nir_def *const z[4], nir_def *out[4])
{
   nir_def *a = nir_fadd(b, z[0], z[2]);
   nir_def *bb = nir_fadd(b, z[0], nir_fneg(b, z[2]));
   nir_def *c = nir_fadd(b, fp24_floor_shift(b, z[1], 1), nir_fneg(b, z[3]));
   nir_def *d = nir_fadd(b, z[1], fp24_floor_shift(b, z[3], 1));

   out[0] = nir_fadd(b, a, d);
   out[1] = nir_fadd(b, bb, c);
   out[2] = nir_fadd(b, bb, nir_fneg(b, c));
   out[3] = nir_fadd(b, a, nir_fneg(b, d));
}

/* Select out[floor(key * 4)] with three predicated moves.  key is the fragment's
 * texcoord component along the selected axis, [0,1] across the four texels, so a
 * fragment centered on texel n carries key = (n + 0.5)/4; the thresholds 0.25,
 * 0.5, 0.75 are dyadic and FP24-exact, and each texel center sits 1/8 from the
 * nearest threshold, so the select is exact under NEAREST.  This is addressing
 * plumbing, not transform arithmetic. */
static nir_def *
select_by_axis(nir_builder *b, nir_def *key, nir_def *const out[4])
{
   nir_def *lo = nir_bcsel(b, nir_flt(b, key, nir_imm_float(b, 0.25f)),
                           out[0], out[1]);
   nir_def *hi = nir_bcsel(b, nir_flt(b, key, nir_imm_float(b, 0.75f)),
                           out[2], out[3]);
   return nir_bcsel(b, nir_flt(b, key, nir_imm_float(b, 0.5f)), lo, hi);
}

/* The four fixed texel-center coordinates (k + 0.5)/4 walked along one axis. */
static nir_def *
texel_center(nir_builder *b, unsigned k)
{
   return nir_imm_float(b, ((float)k + 0.5f) / 4.0f);
}

/* Pass 1: row butterfly.  Fragment (col c, row r) reads row r's four
 * coefficients along texcoord.y and writes rows[r][c]; the column c is selected
 * from texcoord.x.  The write address (c, r) is already the spec's first
 * transpose, so no transpose op is emitted. */
static nir_def *
build_row_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *tcx = nir_channel(b, tc, 0);
   nir_def *tcy = nir_channel(b, tc, 1);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *coord = nir_vec2(b, texel_center(b, k), tcy);
      z[k] = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);
   return nir_replicate(b, select_by_axis(b, tcx, out), 4);
}

/* Pass 2: column butterfly plus the final (h + 32) >> 6 normalize.  Fragment
 * (col c, row i) reads column c of the intermediate along texcoord.x and writes
 * the residual residual[i][c]; the row i is selected from texcoord.y.  The read
 * gathers column c and the select picks i, completing the spec's second
 * transpose in addressing. */
static nir_def *
build_col_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *tcx = nir_channel(b, tc, 0);
   nir_def *tcy = nir_channel(b, tc, 1);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *coord = nir_vec2(b, tcx, texel_center(b, k));
      z[k] = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);
   nir_def *col = select_by_axis(b, tcy, out);
   nir_def *residual =
      fp24_floor_shift(b, nir_fadd(b, col, nir_imm_float(b, 32.0f)), 6);
   return nir_replicate(b, residual, 4);
}

/* Block addressing for the whole-plane transform.  A fragment at normalized
 * along-axis coordinate u over a plane dim texels wide sits at texel
 * px = u*dim - 0.5; the 4x4 block it belongs to starts at texel
 * base4 = 4*floor(px/4), and its position within that block -- expressed as the
 * [0,1)-across-four-texels key select_by_axis consumes -- is
 * key = (px - base4 + 0.5)/4.  dim is a power of two for every H.264 plane (16
 * luma, 8 chroma), so u*dim is the exact half-integer px + 0.5 and base4, key,
 * and the read centers below are all FP24-exact (every value stays well under
 * the 2^17 integer-exact ceiling). */
static void
plane_block_addr(nir_builder *b, nir_def *u, nir_def *dim,
                 nir_def **base4, nir_def **key)
{
   nir_def *uw = nir_fmul(b, u, dim);                 /* px + 0.5 */
   nir_def *bx = fp24_floor_shift(b, uw, 2);          /* floor(px/4) */
   nir_def *b4 = nir_fmul(b, bx, nir_imm_float(b, 4.0f));
   *base4 = b4;
   *key = nir_fmul(b, nir_fadd(b, uw, nir_fneg(b, b4)),
                   nir_imm_float(b, 0.25f));
}

/* Normalized coordinate of coefficient k's texel center within the block whose
 * first texel along the axis is base4: (base4 + k + 0.5) * inv. */
static nir_def *
plane_read_center(nir_builder *b, nir_def *base4, unsigned k, nir_def *inv)
{
   return nir_fmul(b, nir_fadd(b, base4, nir_imm_float(b, (float)k + 0.5f)),
                   inv);
}

/* Pass 1 over a whole plane of tiled 4x4 blocks: the single-block row butterfly
 * with the read column and the output-select key derived from the fragment's
 * plane position instead of a fixed 4-texel window, so one draw transforms every
 * block.  texcoord[0] is (u, v, 1/W, 1/H) and texcoord[1] is (W, H, _, _); the
 * fragment reads its own block's four columns along x at the fixed row v and
 * selects the output column from its in-block x position. */
static nir_def *
build_plane_row_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *dim = fs->texcoord[1];
   nir_def *u = nir_channel(b, tc, 0);
   nir_def *v = nir_channel(b, tc, 1);
   nir_def *inv_w = nir_channel(b, tc, 2);
   nir_def *width = nir_channel(b, dim, 0);

   nir_def *base4, *key;
   plane_block_addr(b, u, width, &base4, &key);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *xc = plane_read_center(b, base4, k, inv_w);
      z[k] = nir_channel(b, vl_nir_tex(fs, 0, nir_vec2(b, xc, v)), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);
   return nir_replicate(b, select_by_axis(b, key, out), 4);
}

/* Pass 2 over the whole plane: the column butterfly plus the (h + 32) >> 6
 * normalize, again with the read row and output-select key derived from the
 * fragment's plane position.  The fragment reads its own block's four rows along
 * y at the fixed column u and selects the output row from its in-block y
 * position, completing the second transpose in addressing. */
static nir_def *
build_plane_col_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *dim = fs->texcoord[1];
   nir_def *u = nir_channel(b, tc, 0);
   nir_def *v = nir_channel(b, tc, 1);
   nir_def *inv_h = nir_channel(b, tc, 3);
   nir_def *height = nir_channel(b, dim, 1);

   nir_def *base4, *key;
   plane_block_addr(b, v, height, &base4, &key);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *yc = plane_read_center(b, base4, k, inv_h);
      z[k] = nir_channel(b, vl_nir_tex(fs, 0, nir_vec2(b, u, yc)), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);
   nir_def *col = select_by_axis(b, key, out);
   nir_def *residual =
      fp24_floor_shift(b, nir_fadd(b, col, nir_imm_float(b, 32.0f)), 6);
   return nir_replicate(b, residual, 4);
}

void *
vl_h264_idct_create_row_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_idct_row_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_row_color(&fs));
}

void *
vl_h264_idct_create_col_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_idct_col_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_col_color(&fs));
}

/* NIR-only entry points for the r300 compile-budget gate: the same row and
 * column kernels built with explicit options and no live screen. */
nir_shader *
vl_h264_idct_row_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 1, "vl:h264_idct_row_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_row_color(&fs));
}

nir_shader *
vl_h264_idct_col_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 1, "vl:h264_idct_col_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_col_color(&fs));
}

void *
vl_h264_idct_create_plane_row_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_idct_plane_row_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_plane_row_color(&fs));
}

void *
vl_h264_idct_create_plane_col_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_idct_plane_col_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_plane_col_color(&fs));
}

nir_shader *
vl_h264_idct_plane_row_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_idct_plane_row_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_plane_row_color(&fs));
}

nir_shader *
vl_h264_idct_plane_col_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_idct_plane_col_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_plane_col_color(&fs));
}
