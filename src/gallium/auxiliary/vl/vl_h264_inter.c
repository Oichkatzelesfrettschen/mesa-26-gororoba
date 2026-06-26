/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

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

/* MV prediction (sec 8.4.1.3) is a neighbour median that the following change
 * fills in.  The zero stub makes mv == mvd, which validates the mvd parse on a
 * macroblock with no available inter neighbour. */
static void
predict_mv(int *mvp_x, int *mvp_y)
{
   *mvp_x = 0;
   *mvp_y = 0;
}

/* Fill every 4x4 block of the partition rectangle [x0, x0+w) x [y0, y0+h), in
 * 4-sample units, with one motion vector and reference index 0. */
static void
fill_part(struct vl_h264_mb_contract *mb, int x0, int y0, int w, int h, int mvx,
          int mvy)
{
   for (int y = y0; y < y0 + h; y++)
      for (int x = x0; x < x0 + w; x++) {
         int blk = y * 4 + x;
         mb->mv_l0[blk][0] = (int16_t)mvx;
         mb->mv_l0[blk][1] = (int16_t)mvy;
         mb->ref_l0[blk] = 0;
      }
}

/* One predicted partition: mv = mvp + mvd, then fill its blocks. */
static void
decode_partition(struct vl_h264_reader *reader, struct vl_h264_mb_contract *mb,
                 int x0, int y0, int w, int h)
{
   int mvp_x, mvp_y;
   predict_mv(&mvp_x, &mvp_y);
   int mvd_x = vl_h264_se(reader);
   int mvd_y = vl_h264_se(reader);
   fill_part(mb, x0, y0, w, h, mvp_x + mvd_x, mvp_y + mvd_y);
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

   /* Skip run (sec 7.3.4): read once, then emit that many P_Skip macroblocks
    * before the next coded macroblock. */
   if (dec->skip_run < 0)
      dec->skip_run = (int)vl_h264_ue(reader);
   if (vl_h264_overrun(reader))
      return VL_H264_P_MB_ERROR;

   if (dec->skip_run > 0) {
      dec->skip_run--;
      int mvp_x, mvp_y;
      predict_mv(&mvp_x, &mvp_y); /* P_Skip MV (sec 8.4.1.1); stub for now */
      fill_part(mb, 0, 0, 4, 4, mvp_x, mvp_y);
      mb->cbp_luma = 0;
      mb->cbp_chroma = 0;
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

   switch (mb_type) {
   case 0: /* P_L0_16x16 */
      decode_partition(reader, mb, 0, 0, 4, 4);
      break;
   case 1: /* P_L0_L0_16x8 */
      decode_partition(reader, mb, 0, 0, 4, 2);
      decode_partition(reader, mb, 0, 2, 4, 2);
      break;
   case 2: /* P_L0_L0_8x16 */
      decode_partition(reader, mb, 0, 0, 2, 4);
      decode_partition(reader, mb, 2, 0, 2, 4);
      break;
   default: { /* 3: P_8x8, 4: P_8x8ref0 */
      unsigned sub[4];
      for (int q = 0; q < 4; q++) {
         sub[q] = vl_h264_ue(reader);
         if (sub[q] > 3)
            return VL_H264_P_MB_ERROR;
      }
      /* ref_idx_l0 is not coded for a single reference. */
      for (int q = 0; q < 4; q++) {
         int qx = (q & 1) * 2, qy = (q >> 1) * 2;
         for (int s = 0; s < sub_part_count[sub[q]]; s++) {
            const struct sub_part *sp = &sub_parts[sub[q]][s];
            decode_partition(reader, mb, qx + sp->x, qy + sp->y, sp->w, sp->h);
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

   if (vl_h264_overrun(reader))
      return VL_H264_P_MB_ERROR;
   return VL_H264_P_MB_INTER;
}
