/*
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"
#include "vl_h264_deblock.h"

static nir_def *
vsub(nir_builder *b, nir_def *x, nir_def *y)
{
   return nir_fadd(b, x, nir_fneg(b, y));
}

static nir_def *
absdiff(nir_builder *b, nir_def *x, nir_def *y)
{
   return nir_fabs(b, vsub(b, x, y));
}

/* Saturating clip to [lo, hi] -- the MOV_SAT-shaped min/max the deblock spec's
 * Clip3 maps to. */
static nir_def *
clip3(nir_builder *b, nir_def *x, nir_def *lo, nir_def *hi)
{
   return nir_fmin(b, nir_fmax(b, x, lo), hi);
}

/* Arithmetic right shift on the shiftless FP24 ALU: floor(z * 2^-k) via scale,
 * FRC, subtract.  FRC floors toward minus infinity, matching the two's-
 * complement shift for the signed filter deltas; the largest intermediate is
 * |((q0-p0)<<2) + (p1-q1) + 4| <= 1279, far under the FP24 ceiling 2^17. */
static nir_def *
fp24_floor_shift(nir_builder *b, nir_def *value, unsigned shift)
{
   nir_def *scaled = nir_fmul(b, value,
                              nir_imm_float(b, 1.0f / (float)(1u << shift)));
   return nir_fadd(b, scaled, nir_fneg(b, nir_ffract(b, scaled)));
}

/* The normal luma deblock filter (ITU-T H.264 sec 8.7.2.3) over six edge samples
 * s = p2,p1,p0,q0,q1,q2, returning the four possibly-modified samples
 * p1',p0',q0',q1' in RGBA.  Every gate is a predicated select and every clip is a
 * saturating min/max, so the filter is one straight-line pass.  The boundary-
 * strength gate is the caller's: a segment with strength zero is not filtered. */
static nir_def *
deblock_filter(nir_builder *b, nir_def *const s[6], nir_def *alpha,
               nir_def *beta, nir_def *tc0)
{
   nir_def *p2 = s[0], *p1 = s[1], *p0 = s[2];
   nir_def *q0 = s[3], *q1 = s[4], *q2 = s[5];

   nir_def *on = nir_iand(b,
      nir_iand(b, nir_flt(b, absdiff(b, p0, q0), alpha),
               nir_flt(b, absdiff(b, p1, p0), beta)),
      nir_flt(b, absdiff(b, q1, q0), beta));
   nir_def *ap = nir_flt(b, absdiff(b, p2, p0), beta);
   nir_def *aq = nir_flt(b, absdiff(b, q2, q0), beta);

   nir_def *tc = nir_fadd(b, nir_fadd(b, tc0, nir_b2f32(b, ap)),
                          nir_b2f32(b, aq));
   nir_def *neg_tc = nir_fneg(b, tc);

   /* delta = clip3(-tc, tc, (((q0-p0)<<2) + (p1-q1) + 4) >> 3) */
   nir_def *raw = fp24_floor_shift(b,
      nir_fadd(b, nir_fadd(b,
                           nir_fmul(b, vsub(b, q0, p0), nir_imm_float(b, 4.0f)),
                           vsub(b, p1, q1)),
               nir_imm_float(b, 4.0f)), 3);
   nir_def *delta = clip3(b, raw, neg_tc, tc);

   nir_def *zero = nir_imm_float(b, 0.0f);
   nir_def *c255 = nir_imm_float(b, 255.0f);
   nir_def *p0n = clip3(b, nir_fadd(b, p0, delta), zero, c255);
   nir_def *q0n = clip3(b, vsub(b, q0, delta), zero, c255);

   /* avg = (p0 + q0 + 1) >> 1 */
   nir_def *avg = fp24_floor_shift(b,
      nir_fadd(b, nir_fadd(b, p0, q0), nir_imm_float(b, 1.0f)), 1);
   nir_def *neg_tc0 = nir_fneg(b, tc0);
   nir_def *dp1 = clip3(b, fp24_floor_shift(b,
      vsub(b, nir_fadd(b, p2, avg), nir_fmul(b, p1, nir_imm_float(b, 2.0f))), 1),
                        neg_tc0, tc0);
   nir_def *dq1 = clip3(b, fp24_floor_shift(b,
      vsub(b, nir_fadd(b, q2, avg), nir_fmul(b, q1, nir_imm_float(b, 2.0f))), 1),
                        neg_tc0, tc0);
   nir_def *p1n = nir_bcsel(b, ap, nir_fadd(b, p1, dp1), p1);
   nir_def *q1n = nir_bcsel(b, aq, nir_fadd(b, q1, dq1), q1);

   return nir_vec4(b,
                   nir_bcsel(b, on, p1n, p1),
                   nir_bcsel(b, on, p0n, p0),
                   nir_bcsel(b, on, q0n, q0),
                   nir_bcsel(b, on, q1n, q1));
}

/* Packed-output deblock kernel: read the six samples across the edge from the
 * fragment's position (texcoord.xy walked by texcoord.zw) and write all four
 * modified samples to RGBA.  The strength gate is the caller's. */
static nir_def *
build_deblock_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *coord = fs->texcoord[0];
   nir_def *par = fs->texcoord[1];
   nir_def *base = nir_vec2(b, nir_channel(b, coord, 0),
                            nir_channel(b, coord, 1));
   nir_def *step = nir_vec2(b, nir_channel(b, coord, 2),
                            nir_channel(b, coord, 3));

   nir_def *s[6];
   for (unsigned k = 0; k < 6; ++k) {
      nir_def *off = nir_fmul(b, step, nir_imm_float(b, (float)k - 2.0f));
      s[k] = nir_channel(b, vl_nir_tex(fs, 0, nir_fadd(b, base, off)), 0);
   }
   return deblock_filter(b, s, nir_channel(b, par, 0), nir_channel(b, par, 1),
                         nir_channel(b, par, 2));
}

/* In-place apply kernel: each fragment of a four-sample-wide edge strip writes
 * its own filtered sample.  The strip's [0,1) local coordinate (second varying
 * .x) places the fragment as p1/p0/q0/q1 -- rel = floor(local*4) - 2 -- so the
 * six edge neighbours sit at offsets j - 3 - rel from the fragment along the
 * edge axis, read relative to the fragment's own position so the addressing is
 * exact at any plane width.  alpha, beta, and tc0 ride in the rest of the second
 * varying; the strip is only drawn where the boundary strength is nonzero. */
static nir_def *
build_deblock_apply_color(struct vl_nir_fs *fs, bool is_vertical)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *par = fs->texcoord[1];
   nir_def *self = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *axis = is_vertical
      ? nir_vec2(b, nir_channel(b, tc, 2), nir_imm_float(b, 0.0f))
      : nir_vec2(b, nir_imm_float(b, 0.0f), nir_channel(b, tc, 3));

   nir_def *local4 = nir_fmul(b, nir_channel(b, par, 0), nir_imm_float(b, 4.0f));
   nir_def *rel = nir_fadd(b, fp24_floor_shift(b, local4, 0),
                           nir_imm_float(b, -2.0f));

   nir_def *s[6];
   for (unsigned k = 0; k < 6; ++k) {
      nir_def *scale = nir_fadd(b, nir_imm_float(b, (float)k - 3.0f),
                                nir_fneg(b, rel));
      nir_def *coord = nir_fadd(b, self, nir_fmul(b, axis, scale));
      s[k] = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
   }

   nir_def *out = deblock_filter(b, s, nir_channel(b, par, 1),
                                 nir_channel(b, par, 2), nir_channel(b, par, 3));
   /* Pick this fragment's lane: rel -2,-1,0,1 -> p1',p0',q0',q1'. */
   nir_def *sel = nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, -1.5f)),
      nir_channel(b, out, 0),
      nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, -0.5f)),
         nir_channel(b, out, 1),
         nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, 0.5f)),
            nir_channel(b, out, 2), nir_channel(b, out, 3))));
   return nir_replicate(b, sel, 4);
}

static nir_def *
fmul_imm(nir_builder *b, nir_def *x, float k)
{
   return nir_fmul(b, x, nir_imm_float(b, k));
}

/* One side of the bS=4 strong luma filter (ITU-T H.264 sec 8.7.2.4) over the
 * eight edge samples s = p3,p2,p1,p0,q0,q1,q2,q3.  The p side writes p2',p1',p0'
 * into out[0..2], the q side q0',q1',q2'; each side takes the three-tap update
 * when its second difference is small and the across-edge difference is below
 * alpha/4 + 2 (strong_thr, passed in so the shader needs no divide), else the
 * one-tap p0/q0 update, under the shared activity gate.  The whole six-output
 * filter as one straight-line kernel schedules to 77 r300 ALU, over the non-HB
 * 64-instruction budget, so the two sides are separate kernels -- each schedules
 * well under 64 -- and the macroblock-boundary edge is drawn as a p strip and a q
 * strip.  Every numerator is a sum of in-range samples, so each result stays in
 * 0..255 with no Clip1, and the largest, 2*p3 + 3*p2 + p1 + p0 + q0 + 4 <= 2044,
 * is far under the FP24 integer-exact ceiling 2^17, so the floors are exact. */
static void
deblock_strong_half(nir_builder *b, nir_def *const s[8], nir_def *alpha,
                    nir_def *beta, nir_def *strong_thr, bool p_side,
                    nir_def *out[3])
{
   nir_def *p2 = s[1], *p1 = s[2], *p0 = s[3];
   nir_def *q0 = s[4], *q1 = s[5];

   nir_def *on = nir_iand(b,
      nir_iand(b, nir_flt(b, absdiff(b, p0, q0), alpha),
               nir_flt(b, absdiff(b, p1, p0), beta)),
      nir_flt(b, absdiff(b, q1, q0), beta));
   nir_def *strong = nir_flt(b, absdiff(b, p0, q0), strong_thr);

   if (p_side) {
      nir_def *p3 = s[0];
      nir_def *ap = nir_flt(b, absdiff(b, p2, p0), beta);
      nir_def *use3 = nir_iand(b, strong, ap);
      /* p0' three-tap = (p2+2p1+2p0+2q0+q1+4)>>3, one-tap = (2p1+p0+q1+2)>>2 */
      nir_def *p0_three = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, nir_fadd(b,
         nir_fadd(b, nir_fadd(b, p2, fmul_imm(b, p1, 2.0f)), fmul_imm(b, p0, 2.0f)),
         fmul_imm(b, q0, 2.0f)), q1), nir_imm_float(b, 4.0f)), 3);
      nir_def *p1_three = fp24_floor_shift(b, nir_fadd(b,
         nir_fadd(b, nir_fadd(b, p2, p1), nir_fadd(b, p0, q0)),
         nir_imm_float(b, 2.0f)), 2);
      nir_def *p2_three = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, nir_fadd(b,
         nir_fadd(b, fmul_imm(b, p3, 2.0f), fmul_imm(b, p2, 3.0f)), p1),
         nir_fadd(b, p0, q0)), nir_imm_float(b, 4.0f)), 3);
      nir_def *p0_one = fp24_floor_shift(b, nir_fadd(b,
         nir_fadd(b, fmul_imm(b, p1, 2.0f), p0),
         nir_fadd(b, q1, nir_imm_float(b, 2.0f))), 2);
      out[0] = nir_bcsel(b, on, nir_bcsel(b, use3, p2_three, p2), p2);
      out[1] = nir_bcsel(b, on, nir_bcsel(b, use3, p1_three, p1), p1);
      out[2] = nir_bcsel(b, on, nir_bcsel(b, use3, p0_three, p0_one), p0);
   } else {
      nir_def *q2 = s[6], *q3 = s[7];
      nir_def *aq = nir_flt(b, absdiff(b, q2, q0), beta);
      nir_def *use3 = nir_iand(b, strong, aq);
      nir_def *q0_three = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, nir_fadd(b,
         nir_fadd(b, nir_fadd(b, q2, fmul_imm(b, q1, 2.0f)), fmul_imm(b, q0, 2.0f)),
         fmul_imm(b, p0, 2.0f)), p1), nir_imm_float(b, 4.0f)), 3);
      nir_def *q1_three = fp24_floor_shift(b, nir_fadd(b,
         nir_fadd(b, nir_fadd(b, q2, q1), nir_fadd(b, q0, p0)),
         nir_imm_float(b, 2.0f)), 2);
      nir_def *q2_three = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, nir_fadd(b,
         nir_fadd(b, fmul_imm(b, q3, 2.0f), fmul_imm(b, q2, 3.0f)), q1),
         nir_fadd(b, q0, p0)), nir_imm_float(b, 4.0f)), 3);
      nir_def *q0_one = fp24_floor_shift(b, nir_fadd(b,
         nir_fadd(b, fmul_imm(b, q1, 2.0f), q0),
         nir_fadd(b, p1, nir_imm_float(b, 2.0f))), 2);
      out[0] = nir_bcsel(b, on, nir_bcsel(b, use3, q0_three, q0_one), q0);
      out[1] = nir_bcsel(b, on, nir_bcsel(b, use3, q1_three, q1), q1);
      out[2] = nir_bcsel(b, on, nir_bcsel(b, use3, q2_three, q2), q2);
   }
}

/* In-place strong-filter apply kernel for one side: each fragment of a
 * three-sample-wide strip writes its own filtered output (p2',p1',p0' on the p
 * side, q0',q1',q2' on the q side).  rel = floor(local*3) in 0..2 places the
 * fragment among the three outputs; the fragment's global edge index is rel + base
 * (base 1 for the p side, whose outputs are p2,p1,p0 = the 2nd..4th of p3..q3, 4
 * for the q side), so the eight inputs sit at offsets j - rel - base, read relative
 * to the fragment.  par = (local, alpha, beta, strong_thr). */
static nir_def *
build_deblock_strong_side(struct vl_nir_fs *fs, bool is_vertical, bool p_side)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *par = fs->texcoord[1];
   nir_def *self = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *axis = is_vertical
      ? nir_vec2(b, nir_channel(b, tc, 2), nir_imm_float(b, 0.0f))
      : nir_vec2(b, nir_imm_float(b, 0.0f), nir_channel(b, tc, 3));

   nir_def *local3 = nir_fmul(b, nir_channel(b, par, 0), nir_imm_float(b, 3.0f));
   nir_def *rel = fp24_floor_shift(b, local3, 0);
   const float base = p_side ? 1.0f : 4.0f;

   nir_def *s[8];
   for (unsigned j = 0; j < 8; ++j) {
      nir_def *scale = nir_fadd(b, nir_imm_float(b, (float)j - base),
                                nir_fneg(b, rel));
      nir_def *coord = nir_fadd(b, self, nir_fmul(b, axis, scale));
      s[j] = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
   }

   nir_def *out[3];
   deblock_strong_half(b, s, nir_channel(b, par, 1), nir_channel(b, par, 2),
                       nir_channel(b, par, 3), p_side, out);
   /* Pick this fragment's lane: rel 0,1,2 -> out[0],out[1],out[2]. */
   nir_def *sel = nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, 0.5f)),
      out[0],
      nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, 1.5f)), out[1], out[2]));
   return nir_replicate(b, sel, 4);
}

/* In-place chroma deblock apply kernel (ITU-T H.264 sec 8.7.2.3/4,
 * chromaEdgeFlag=1).  Each fragment of a two-sample-wide strip writes its own
 * filtered output -- p0' at rel 0, q0' at rel 1 -- reading the four inputs
 * p1,p0,q0,q1 relative to its own position.  Chroma writes only p0 and q0: the
 * normal filter (bS 1..3) uses the same delta as luma but with tC = tC0 + 1 and no
 * p1/q1 update, and the bS=4 filter is the unconditional two-tap average
 * p0'=(2p1+p0+q1+2)>>2, with no strong/weak split.  par = (local, alpha, beta,
 * tC); for the strong kernel the fourth lane is unused.  The activity gate is the
 * luma gate.  The largest intermediate, the normal delta numerator
 * |((q0-p0)<<2)+(p1-q1)+4| <= 1279, stays under the FP24 ceiling 2^17. */
static nir_def *
build_chroma_deblock_apply(struct vl_nir_fs *fs, bool is_vertical, bool is_strong)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *par = fs->texcoord[1];
   nir_def *self = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *axis = is_vertical
      ? nir_vec2(b, nir_channel(b, tc, 2), nir_imm_float(b, 0.0f))
      : nir_vec2(b, nir_imm_float(b, 0.0f), nir_channel(b, tc, 3));

   nir_def *local2 = nir_fmul(b, nir_channel(b, par, 0), nir_imm_float(b, 2.0f));
   nir_def *rel = fp24_floor_shift(b, local2, 0);   /* 0 -> p0, 1 -> q0 */

   nir_def *s[4];
   for (unsigned i = 0; i < 4; ++i) {
      nir_def *scale = nir_fadd(b, nir_imm_float(b, (float)i - 1.0f),
                                nir_fneg(b, rel));
      nir_def *coord = nir_fadd(b, self, nir_fmul(b, axis, scale));
      s[i] = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
   }
   nir_def *p1 = s[0], *p0 = s[1], *q0 = s[2], *q1 = s[3];
   nir_def *alpha = nir_channel(b, par, 1), *beta = nir_channel(b, par, 2);

   nir_def *on = nir_iand(b,
      nir_iand(b, nir_flt(b, absdiff(b, p0, q0), alpha),
               nir_flt(b, absdiff(b, p1, p0), beta)),
      nir_flt(b, absdiff(b, q1, q0), beta));

   nir_def *p0n, *q0n;
   if (is_strong) {
      p0n = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, fmul_imm(b, p1, 2.0f), p0),
                                         nir_fadd(b, q1, nir_imm_float(b, 2.0f))), 2);
      q0n = fp24_floor_shift(b, nir_fadd(b, nir_fadd(b, fmul_imm(b, q1, 2.0f), q0),
                                         nir_fadd(b, p1, nir_imm_float(b, 2.0f))), 2);
   } else {
      nir_def *tc_v = nir_channel(b, par, 3);    /* tC = tC0 + 1 (caller-precomputed) */
      nir_def *raw = fp24_floor_shift(b,
         nir_fadd(b, nir_fadd(b,
                              nir_fmul(b, vsub(b, q0, p0), nir_imm_float(b, 4.0f)),
                              vsub(b, p1, q1)),
                  nir_imm_float(b, 4.0f)), 3);
      nir_def *delta = clip3(b, raw, nir_fneg(b, tc_v), tc_v);
      nir_def *zero = nir_imm_float(b, 0.0f), *c255 = nir_imm_float(b, 255.0f);
      p0n = clip3(b, nir_fadd(b, p0, delta), zero, c255);
      q0n = clip3(b, vsub(b, q0, delta), zero, c255);
   }

   nir_def *p0o = nir_bcsel(b, on, p0n, p0);
   nir_def *q0o = nir_bcsel(b, on, q0n, q0);
   nir_def *sel = nir_bcsel(b, nir_flt(b, rel, nir_imm_float(b, 0.5f)), p0o, q0o);
   return nir_replicate(b, sel, 4);
}

void *
vl_h264_deblock_create_luma_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_luma_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_color(&fs));
}

void *
vl_h264_deblock_create_apply_v_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_apply_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_apply_color(&fs, true));
}

void *
vl_h264_deblock_create_apply_h_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_apply_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_apply_color(&fs, false));
}

void *
vl_h264_deblock_create_strong_vp_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_strong_vp_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_strong_side(&fs, true, true));
}

void *
vl_h264_deblock_create_strong_vq_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_strong_vq_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_strong_side(&fs, true, false));
}

void *
vl_h264_deblock_create_strong_hp_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_strong_hp_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_strong_side(&fs, false, true));
}

void *
vl_h264_deblock_create_strong_hq_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_strong_hq_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_deblock_strong_side(&fs, false, false));
}

void *
vl_h264_deblock_create_chroma_v_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_chroma_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_chroma_deblock_apply(&fs, true, false));
}

void *
vl_h264_deblock_create_chroma_h_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_chroma_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_chroma_deblock_apply(&fs, false, false));
}

void *
vl_h264_deblock_create_chroma_strong_v_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_chroma_strong_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_chroma_deblock_apply(&fs, true, true));
}

void *
vl_h264_deblock_create_chroma_strong_h_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_deblock_chroma_strong_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_chroma_deblock_apply(&fs, false, true));
}

nir_shader *
vl_h264_deblock_luma_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_luma_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_color(&fs));
}

nir_shader *
vl_h264_deblock_apply_v_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_apply_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_apply_color(&fs, true));
}

nir_shader *
vl_h264_deblock_apply_h_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_apply_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_apply_color(&fs, false));
}

nir_shader *
vl_h264_deblock_strong_vp_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_strong_vp_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_strong_side(&fs, true, true));
}

nir_shader *
vl_h264_deblock_strong_vq_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_strong_vq_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_strong_side(&fs, true, false));
}

nir_shader *
vl_h264_deblock_strong_hp_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_strong_hp_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_strong_side(&fs, false, true));
}

nir_shader *
vl_h264_deblock_strong_hq_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_strong_hq_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_deblock_strong_side(&fs, false, false));
}

nir_shader *
vl_h264_deblock_chroma_v_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_chroma_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_chroma_deblock_apply(&fs, true, false));
}

nir_shader *
vl_h264_deblock_chroma_h_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_chroma_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_chroma_deblock_apply(&fs, false, false));
}

nir_shader *
vl_h264_deblock_chroma_strong_v_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_chroma_strong_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_chroma_deblock_apply(&fs, true, true));
}

nir_shader *
vl_h264_deblock_chroma_strong_h_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_deblock_chroma_strong_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_chroma_deblock_apply(&fs, false, true));
}
