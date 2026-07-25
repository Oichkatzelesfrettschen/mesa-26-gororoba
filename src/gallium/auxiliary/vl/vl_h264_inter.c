/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "pipe/p_video_state.h"

#include "vl_h264_inter.h"

/*
 * Clean-room from ITU-T H.264 sec 7.3.4, 7.3.5, 7.3.5.1, 7.3.5.2 (P-slice
 * macroblock syntax) and Table 9-4 (the inter coded_block_pattern mapping).  The
 * clip is Constrained Baseline with a single reference (max_num_ref_frames 1), so
 * ref_idx_l0 is always 0 and is not coded, and there is no list 1.
 */

/* Table 9-4 inter column: me(v) codeNum to coded_block_pattern for inter
 * prediction modes.  The intra column lives in vl_h264_mb_decode.c. */
static const uint8_t inter_cbp_table[48] = {
    0, 16,  1,  2,  4,  8, 32,  3,  5, 10, 12, 15, 47,  7, 11, 13,
   14,  6,  9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
   17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41,
};

static int
median3(int a, int b, int c)
{
   int lo = a < b ? a : b;
   lo = lo < c ? lo : c;
   int hi = a > b ? a : b;
   hi = hi > c ? hi : c;
   return a + b + c - lo - hi;
}

/* Read a neighbor 4x4 block's list-0 motion vector and reference from the frame
 * store.  (bx, by) is the within-macroblock 4x4 position and may be -1 or 4 for a
 * neighboring macroblock.  Returns false when the block is outside the frame, in
 * an earlier slice, or not yet decoded; an available intra block returns
 * reference -1 and a zero vector. */
static bool
neighbor_mv(const struct vl_h264_mb_decoder *dec, unsigned cur, unsigned mb_x,
             unsigned mb_y, int bx, int by, int *mvx, int *mvy, int *ref)
{
   int abx = (int)mb_x * 4 + bx, aby = (int)mb_y * 4 + by;
   if (abx < 0 || aby < 0 || abx >= (int)dec->width_in_mbs * 4 ||
       aby >= (int)dec->height_in_mbs * 4)
      return false;
   unsigned nmb = ((unsigned)aby / 4) * dec->width_in_mbs + (unsigned)abx / 4;
   unsigned fi = nmb * 16 + ((unsigned)aby % 4) * 4 + (unsigned)abx % 4;
   if (nmb > cur)
      return false;
   /* A neighbor in an earlier slice is not available for prediction, the same
    * rule the intra mode and sample neighbors follow. */
   if (nmb < dec->slice->first_mb_in_slice)
      return false;
   if (nmb == cur && dec->ref_l0_frame[fi] < 0)
      return false; /* in this macroblock but a not-yet-decoded partition */
   *ref = dec->ref_l0_frame[fi];
   if (*ref < 0) {
      *mvx = *mvy = 0;
   } else {
      *mvx = dec->mv_l0_frame[fi * 2];
      *mvy = dec->mv_l0_frame[fi * 2 + 1];
   }
   return true;
}

/* The neighbor a 16x8 or 8x16 partition takes directly when its reference
 * matches (the directional segmentation prediction of sec 8.4.1.3). */
enum mvp_pref { PREF_NONE, PREF_A, PREF_B, PREF_C };

/* Motion vector prediction for one partition (sec 8.4.1.3): neighbors A (left),
 * B (above), and C (above-right, falling back to D above-left), then the
 * directional special case or the median (sec 8.4.1.3.1).  The reference is
 * always 0 (single reference), so an intra or absent neighbor never matches. */
static void
predict_mv(const struct vl_h264_mb_decoder *dec, unsigned cur, unsigned mb_x,
           unsigned mb_y, int px, int py, int pw, enum mvp_pref pref,
           int ref_idx, int *mvp_x, int *mvp_y)
{
   int ax, ay, aref, bx, by, bref, cx, cy, cref;
   bool av_a = neighbor_mv(dec, cur, mb_x, mb_y, px - 1, py, &ax, &ay, &aref);
   bool av_b = neighbor_mv(dec, cur, mb_x, mb_y, px, py - 1, &bx, &by, &bref);
   bool av_c = neighbor_mv(dec, cur, mb_x, mb_y, px + pw, py - 1, &cx, &cy, &cref);
   if (!av_c)
      av_c = neighbor_mv(dec, cur, mb_x, mb_y, px - 1, py - 1, &cx, &cy, &cref);

   /* When B and C are both unavailable but A is, A stands in for them (8-160). */
   if (!av_b && !av_c && av_a) {
      bx = cx = ax;
      by = cy = ay;
      bref = cref = aref;
      av_b = av_c = true;
   }
   if (!av_a) { ax = ay = 0; aref = -1; }
   if (!av_b) { bx = by = 0; bref = -1; }
   if (!av_c) { cx = cy = 0; cref = -1; }

   if (pref == PREF_A && aref == ref_idx) { *mvp_x = ax; *mvp_y = ay; return; }
   if (pref == PREF_B && bref == ref_idx) { *mvp_x = bx; *mvp_y = by; return; }
   if (pref == PREF_C && cref == ref_idx) { *mvp_x = cx; *mvp_y = cy; return; }

   /* The unique matching reference, else the component-wise median (8-164/8-165). */
   int match = (aref == ref_idx) + (bref == ref_idx) + (cref == ref_idx);
   if (match == 1) {
      *mvp_x = aref == ref_idx ? ax : bref == ref_idx ? bx : cx;
      *mvp_y = aref == ref_idx ? ay : bref == ref_idx ? by : cy;
   } else {
      *mvp_x = median3(ax, bx, cx);
      *mvp_y = median3(ay, by, cy);
   }
}

/* Fill the partition rectangle [x0, x0+w) x [y0, y0+h), in 4-sample units, in
 * both the contract and the frame store, so the next partition's prediction
 * reads it. */
static void
fill_part(struct vl_h264_mb_decoder *dec, unsigned cur,
          struct vl_h264_mb_contract *mb, int x0, int y0, int w, int h, int mvx,
          int mvy, int ref_idx)
{
   for (int y = y0; y < y0 + h; y++)
      for (int x = x0; x < x0 + w; x++) {
         int blk = y * 4 + x;
         mb->mv_l0[blk][0] = (int16_t)mvx;
         mb->mv_l0[blk][1] = (int16_t)mvy;
         mb->ref_l0[blk] = (int8_t)ref_idx;
         dec->mv_l0_frame[(cur * 16 + blk) * 2] = (int16_t)mvx;
         dec->mv_l0_frame[(cur * 16 + blk) * 2 + 1] = (int16_t)mvy;
         dec->ref_l0_frame[cur * 16 + blk] = (int8_t)ref_idx;
      }
}

/* ref_idx_l0 (sec 7.4.5.1): te(v) collapses to one inverted bit when only two
 * references are active, ue(v) for more, and is not coded for one. */
static int
read_ref_idx_l0(struct vl_h264_mb_decoder *dec, struct vl_h264_reader *reader)
{
   unsigned active = dec->slice->num_ref_idx_l0_active;
   if (active <= 1)
      return 0;
   if (active == 2)
      return !vl_h264_u(reader, 1);
   return (int)vl_h264_ue(reader);
}

/* One predicted partition: mv = mvp + mvd, filled into the contract. */
static void
decode_partition(struct vl_h264_mb_decoder *dec, unsigned cur,
                 struct vl_h264_reader *reader, unsigned mb_x, unsigned mb_y,
                 struct vl_h264_mb_contract *mb, int x0, int y0, int w, int h,
                 enum mvp_pref pref, int ref_idx)
{
   int mvp_x, mvp_y;
   predict_mv(dec, cur, mb_x, mb_y, x0, y0, w, pref, ref_idx, &mvp_x, &mvp_y);
   int mvd_x = vl_h264_se(reader);
   int mvd_y = vl_h264_se(reader);
   fill_part(dec, cur, mb, x0, y0, w, h, mvp_x + mvd_x, mvp_y + mvd_y, ref_idx);
}

/* The four P sub-macroblock types (Table 7-17) as their sub-partition rectangles
 * within an 8x8 sub-macroblock, in 4-sample units: 8x8, 8x4, 4x8, 4x4. */
struct sub_part { int x, y, w, h; };
static const struct sub_part sub_parts[4][4] = {
   { { 0, 0, 2, 2 } },                                              /* P_L0_8x8 */
   { { 0, 0, 2, 1 }, { 0, 1, 2, 1 } },                             /* P_L0_8x4 */
   { { 0, 0, 1, 2 }, { 1, 0, 1, 2 } },                             /* P_L0_4x8 */
   { { 0, 0, 1, 1 }, { 1, 0, 1, 1 }, { 0, 1, 1, 1 }, { 1, 1, 1, 1 } }, /* 4x4 */
};
static const int sub_part_count[4] = { 1, 2, 2, 4 };

static void
mark_no_coeffs(struct vl_h264_mb_decoder *dec, unsigned mb_x, unsigned mb_y)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;
   for (unsigned blk = 0; blk < 16; blk++)
      dec->nz_luma[cur * 16 + blk] = 0;
   for (unsigned comp = 0; comp < 2; comp++)
      for (unsigned blk = 0; blk < 4; blk++)
         dec->nz_chroma_ac[(cur * 2 + comp) * 4 + blk] = 0;
}

enum vl_h264_p_mb_kind
vl_h264_decode_p_mb(struct vl_h264_mb_decoder *dec, struct vl_h264_reader *reader,
                    unsigned mb_x, unsigned mb_y, struct vl_h264_mb_contract *mb)
{
   mb->mb_x = (int32_t)mb_x;
   mb->mb_y = (int32_t)mb_y;
   mb->slice_type = dec->slice->slice_type;
   mb->transform_8x8 = 0;
   mb->mb_type = 0;
   mb->intra_chroma_pred_mode = 0;
   for (int i = 0; i < 16; i++) {
      mb->ref_l0[i] = -1;
      mb->ref_l1[i] = -1;
      mb->mv_l0[i][0] = mb->mv_l0[i][1] = 0;
      mb->mv_l1[i][0] = mb->mv_l1[i][1] = 0;
   }

   unsigned cur = mb_y * dec->width_in_mbs + mb_x;

   /* Skip run (sec 7.3.4): read once, then emit that many P_Skip macroblocks
    * before the next coded macroblock. */
   if (dec->skip_run < 0)
      dec->skip_run = (int)vl_h264_ue(reader);
   if (vl_h264_overrun(reader))
      return VL_H264_P_MB_ERROR;

   if (dec->skip_run > 0) {
      dec->skip_run--;
      /* P_Skip motion vector (sec 8.4.1.1): zero when the left or top neighbor
       * is unavailable or is a zero-vector reference-0 block, otherwise the
       * ordinary 16x16 prediction. */
      int ax, ay, aref, bx, by, bref, mvx = 0, mvy = 0;
      bool av_a = neighbor_mv(dec, cur, mb_x, mb_y, -1, 0, &ax, &ay, &aref);
      bool av_b = neighbor_mv(dec, cur, mb_x, mb_y, 0, -1, &bx, &by, &bref);
      if (av_a && av_b && !(aref == 0 && ax == 0 && ay == 0) &&
          !(bref == 0 && bx == 0 && by == 0))
         predict_mv(dec, cur, mb_x, mb_y, 0, 0, 4, PREF_NONE, 0, &mvx, &mvy);
      fill_part(dec, cur, mb, 0, 0, 4, 4, mvx, mvy, 0);
      mb->cbp_luma = 0;
      mb->cbp_chroma = 0;
      /* A skipped macroblock has no mb_qp_delta and inherits the running QP.  The
       * deblock reads qp_y/qp_cb/qp_cr per macroblock for its alpha/beta/tc0
       * thresholds, so without this the skip leaves them at zero and the edges it
       * borders are filtered at the wrong QP index. */
      mb->qp_y = dec->qp_y;
      mb->qp_cb = vl_h264_chroma_qp_from_luma(dec->qp_y,
                                              dec->pps->chroma_qp_index_offset);
      mb->qp_cr = vl_h264_chroma_qp_from_luma(dec->qp_y,
                                              dec->pps->second_chroma_qp_index_offset);
      mark_no_coeffs(dec, mb_x, mb_y);
      return VL_H264_P_MB_SKIP;
   }
   dec->skip_run = -1;

   unsigned mb_type = vl_h264_ue(reader);
   if (vl_h264_overrun(reader))
      return VL_H264_P_MB_ERROR;

   /* mb_type 5 and above are the intra types offset past the five P types.  The
    * shared intra body decodes them; ref_l0 stays -1, the intra marker the GPU
    * back half keys on, and the residual stage handles them like an I-slice
    * intra macroblock. */
   if (mb_type >= 5) {
      if (!vl_h264_decode_intra_mb_body(dec, reader, mb_x, mb_y, mb, mb_type - 5))
         return VL_H264_P_MB_ERROR;
      return VL_H264_P_MB_INTRA;
   }

   /* mb_pred (sec 7.3.5.1) codes ref_idx_l0 for every partition before any
    * mvd_l0, so parse the reference indices first, then the motion vectors. */
   switch (mb_type) {
   case 0: { /* P_L0_16x16 */
      int r = read_ref_idx_l0(dec, reader);
      decode_partition(dec, cur, reader, mb_x, mb_y, mb, 0, 0, 4, 4, PREF_NONE, r);
      break;
   }
   case 1: { /* P_L0_L0_16x8: the top takes B, the bottom A (8-156, 8-157) */
      int r0 = read_ref_idx_l0(dec, reader), r1 = read_ref_idx_l0(dec, reader);
      decode_partition(dec, cur, reader, mb_x, mb_y, mb, 0, 0, 4, 2, PREF_B, r0);
      decode_partition(dec, cur, reader, mb_x, mb_y, mb, 0, 2, 4, 2, PREF_A, r1);
      break;
   }
   case 2: { /* P_L0_L0_8x16: the left takes A, the right C (8-158, 8-159) */
      int r0 = read_ref_idx_l0(dec, reader), r1 = read_ref_idx_l0(dec, reader);
      decode_partition(dec, cur, reader, mb_x, mb_y, mb, 0, 0, 2, 4, PREF_A, r0);
      decode_partition(dec, cur, reader, mb_x, mb_y, mb, 2, 0, 2, 4, PREF_C, r1);
      break;
   }
   default: { /* 3: P_8x8, 4: P_8x8ref0 */
      unsigned sub[4];
      for (int q = 0; q < 4; q++) {
         sub[q] = vl_h264_ue(reader);
         if (sub[q] > 3)
            return VL_H264_P_MB_ERROR;
      }
      /* sub_mb_pred (sec 7.3.5.2) codes one ref_idx_l0 per 8x8 sub-macroblock
       * between sub_mb_type and the mvds; P_8x8ref0 (mb_type 4) leaves them 0. */
      int ref[4] = { 0, 0, 0, 0 };
      if (mb_type != 4)
         for (int q = 0; q < 4; q++)
            ref[q] = read_ref_idx_l0(dec, reader);
      for (int q = 0; q < 4; q++) {
         int qx = (q & 1) * 2, qy = (q >> 1) * 2;
         for (int s = 0; s < sub_part_count[sub[q]]; s++) {
            const struct sub_part *sp = &sub_parts[sub[q]][s];
            decode_partition(dec, cur, reader, mb_x, mb_y, mb, qx + sp->x,
                             qy + sp->y, sp->w, sp->h, PREF_NONE, ref[q]);
         }
      }
      break;
   }
   }

   unsigned code = vl_h264_ue(reader);
   unsigned cbp = code < 48 ? inter_cbp_table[code] : 0;
   mb->cbp_luma = cbp & 0x0f;
   mb->cbp_chroma = cbp >> 4;

   if (mb->cbp_luma || mb->cbp_chroma) {
      int delta = vl_h264_se(reader);
      if (delta < -26 || delta > 25)
         return VL_H264_P_MB_ERROR;
      dec->qp_y = (dec->qp_y + delta + 52) % 52;
   }
   mb->qp_y = dec->qp_y;
   mb->qp_cb = vl_h264_chroma_qp_from_luma(dec->qp_y,
                                           dec->pps->chroma_qp_index_offset);
   mb->qp_cr = vl_h264_chroma_qp_from_luma(dec->qp_y,
                                           dec->pps->second_chroma_qp_index_offset);

   if (vl_h264_overrun(reader))
      return VL_H264_P_MB_ERROR;
   return VL_H264_P_MB_INTER;
}
