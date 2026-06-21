/*
 * Copyright (c) 2026 Terascale Functionalists
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
void *
vl_h264_idct_create_row_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_idct_row_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   nir_builder *b = &fs.b;

   nir_def *tc = fs.texcoord[0];
   nir_def *tcx = nir_channel(b, tc, 0);
   nir_def *tcy = nir_channel(b, tc, 1);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *coord = nir_vec2(b, texel_center(b, k), tcy);
      z[k] = nir_channel(b, vl_nir_tex(&fs, 0, coord), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);

   nir_def *row = select_by_axis(b, tcx, out);
   return vl_nir_fs_finish(&fs, pipe, nir_replicate(b, row, 4));
}

/* Pass 2: column butterfly plus the final (h + 32) >> 6 normalize.  Fragment
 * (col c, row i) reads column c of the intermediate along texcoord.x and writes
 * the residual residual[i][c]; the row i is selected from texcoord.y.  The read
 * gathers column c and the select picks i, completing the spec's second
 * transpose in addressing. */
void *
vl_h264_idct_create_col_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_idct_col_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   nir_builder *b = &fs.b;

   nir_def *tc = fs.texcoord[0];
   nir_def *tcx = nir_channel(b, tc, 0);
   nir_def *tcy = nir_channel(b, tc, 1);

   nir_def *z[4];
   for (unsigned k = 0; k < 4; ++k) {
      nir_def *coord = nir_vec2(b, tcx, texel_center(b, k));
      z[k] = nir_channel(b, vl_nir_tex(&fs, 0, coord), 0);
   }

   nir_def *out[4];
   idct4_butterfly(b, z, out);

   nir_def *col = select_by_axis(b, tcy, out);
   nir_def *residual =
      fp24_floor_shift(b, nir_fadd(b, col, nir_imm_float(b, 32.0f)), 6);
   return vl_nir_fs_finish(&fs, pipe, nir_replicate(b, residual, 4));
}
