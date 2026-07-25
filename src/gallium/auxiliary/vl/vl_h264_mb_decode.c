/*
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <string.h>

#include "pipe/p_video_state.h"
#include "util/u_memory.h"

#include "vl_h264_mb_decode.h"

/*
 * Clean-room from ITU-T H.264 sec 7.3.5/7.4.5 (macroblock layer and types), sec
 * 8.3.1.1 (Intra_4x4 mode prediction), sec 9.1.2 (coded_block_pattern mapping),
 * and sec 8.5.8 (chroma QP).  No third-party decoder source was consulted.
 */

/* Macroblock-type boundaries for an I slice (Table 7-11). */
#define I_NXN_MB_TYPE 0
#define I_PCM_MB_TYPE 25

/* coded_block_pattern me(v) mapping for ChromaArrayType 1/2 (Table 9-4), the
 * Intra_4x4/Intra_8x8 column.  Index is codeNum, value is the 6-bit pattern
 * (low 4 bits luma 8x8 quadrants, high 2 bits chroma). */
static const uint8_t intra_cbp_table[48] = {
   47, 31, 15,  0, 23, 27, 29, 30,  7, 11, 13, 14, 39, 43, 45, 46,
   16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,  1,  2,  4,
    8, 17, 18, 20, 24,  6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41,
};

/* 4x4 luma block scan to position in 4-sample units (sec 6.4.3). */
static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

/* Chroma QP from luma QP (Table 8-15), for qPI in [30, 51]; below 30 it is the
 * identity. */
static const uint8_t chroma_qp_30_51[22] = {
   29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
   36, 37, 37, 38, 38, 39, 39, 39, 39, 39, 39,
};

#define MODE_UNAVAILABLE 99

static unsigned
xy_to_blk(unsigned bx, unsigned by)
{
   for (unsigned i = 0; i < 16; i++)
      if (blk_x[i] == bx && blk_y[i] == by)
         return i;
   return 0;
}

static unsigned
chroma_array_type(const struct pipe_h264_sps *sps)
{
   return sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
}

int
vl_h264_chroma_qp_from_luma(int qp_y, int offset)
{
   int qpi = qp_y + offset;
   if (qpi < 0)
      qpi = 0;
   if (qpi > 51)
      qpi = 51;
   return qpi < 30 ? qpi : chroma_qp_30_51[qpi - 30];
}

/* The stored Intra_4x4 mode of a block, or DC (2) for a block whose macroblock
 * is not Intra_4x4 (sec 8.3.1.1). */
static int
stored_block_mode(struct vl_h264_mb_decoder *dec, unsigned mb_raster,
                  unsigned bx, unsigned by)
{
   int mode = dec->intra4x4_modes[mb_raster * 16 + xy_to_blk(bx, by)];
   return mode >= 0 ? mode : 2;
}

/* The Intra_4x4 mode contributed by a neighbor macroblock's block, or
 * MODE_UNAVAILABLE when that neighbor is outside the frame or the slice, or --
 * under constrained_intra_pred -- when the neighbor is an inter macroblock.  An
 * unavailable neighbor sets dcPredModePredictedFlag, forcing predIntra to DC;
 * an inter neighbor under constrained_intra_pred must set it the same way, so an
 * inter neighbor does not pull the prediction toward its own (non-Intra_4x4)
 * mode.  A non-negative list-0 reference index marks an inter macroblock. */
static int
neighbour_block_mode(struct vl_h264_mb_decoder *dec, unsigned cur_mb_addr,
                     int nb_mb_x, int nb_mb_y, unsigned bx, unsigned by)
{
   if (nb_mb_x < 0 || nb_mb_y < 0 ||
       (unsigned)nb_mb_x >= dec->width_in_mbs ||
       (unsigned)nb_mb_y >= dec->height_in_mbs)
      return MODE_UNAVAILABLE;

   unsigned nb_mb_addr = nb_mb_y * dec->width_in_mbs + nb_mb_x;
   if (nb_mb_addr < dec->slice->first_mb_in_slice || nb_mb_addr >= cur_mb_addr)
      return MODE_UNAVAILABLE;

   if (dec->pps->constrained_intra_pred_flag &&
       dec->ref_l0_frame[nb_mb_addr * 16] >= 0)
      return MODE_UNAVAILABLE;

   return stored_block_mode(dec, nb_mb_addr, bx, by);
}

/* predIntra4x4PredMode for one block (sec 8.3.1.1): the minimum of the left and
 * top neighbor modes, or DC when either neighbor is unavailable. */
static int
predict_intra4x4_mode(struct vl_h264_mb_decoder *dec, unsigned mb_x,
                      unsigned mb_y, unsigned bx, unsigned by)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;
   int left = bx > 0 ? stored_block_mode(dec, cur, bx - 1, by)
                     : neighbour_block_mode(dec, cur, (int)mb_x - 1, mb_y, 3, by);
   int top = by > 0 ? stored_block_mode(dec, cur, bx, by - 1)
                    : neighbour_block_mode(dec, cur, mb_x, (int)mb_y - 1, bx, 3);

   if (left == MODE_UNAVAILABLE || top == MODE_UNAVAILABLE)
      return 2;
   return left < top ? left : top;
}

/* mb_pred for an Intra_4x4 macroblock (sec 7.3.5.1): the 16 luma modes (with the
 * neighbor-predicted derivation) then the chroma mode. */
static void
decode_intra4x4_pred(struct vl_h264_mb_decoder *dec,
                     struct vl_h264_reader *reader, unsigned mb_x,
                     unsigned mb_y, struct vl_h264_mb_contract *mb)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;

   for (unsigned i = 0; i < 16; i++) {
      int predicted = predict_intra4x4_mode(dec, mb_x, mb_y, blk_x[i], blk_y[i]);
      int mode;
      if (vl_h264_u(reader, 1)) {        /* prev_intra4x4_pred_mode_flag */
         mode = predicted;
      } else {
         unsigned rem = vl_h264_u(reader, 3);  /* rem_intra4x4_pred_mode */
         mode = (int)rem < predicted ? (int)rem : (int)rem + 1;
      }
      mb->intra4x4_pred_mode[i] = (uint8_t)mode;
      dec->intra4x4_modes[cur * 16 + i] = (int8_t)mode;
   }

   if (chroma_array_type(dec->sps) == 1 || chroma_array_type(dec->sps) == 2)
      mb->intra_chroma_pred_mode = (uint8_t)vl_h264_ue(reader);
}

/* Mark every 4x4 block of a macroblock as not Intra_4x4 (DC-predicting for a
 * neighbor), used for I_16x16 and, later, inter macroblocks. */
static void
clear_intra4x4_modes(struct vl_h264_mb_decoder *dec, unsigned mb_x,
                     unsigned mb_y)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;
   for (unsigned i = 0; i < 16; i++)
      dec->intra4x4_modes[cur * 16 + i] = -1;
}

bool
vl_h264_mb_decoder_init(struct vl_h264_mb_decoder *dec,
                        const struct pipe_h264_picture_desc *picture,
                        unsigned width_in_mbs, unsigned height_in_mbs)
{
   memset(dec, 0, sizeof(*dec));

   if (!picture || !picture->pps || !picture->pps->sps ||
       width_in_mbs == 0 || height_in_mbs == 0)
      return false;

   dec->picture = picture;
   dec->pps = picture->pps;
   dec->sps = picture->pps->sps;
   dec->width_in_mbs = width_in_mbs;
   dec->height_in_mbs = height_in_mbs;
   dec->num_mbs = width_in_mbs * height_in_mbs;

   dec->intra4x4_modes = MALLOC(dec->num_mbs * 16 * sizeof(*dec->intra4x4_modes));
   dec->nz_luma = CALLOC(dec->num_mbs * 16, sizeof(*dec->nz_luma));
   dec->nz_chroma_ac = CALLOC(dec->num_mbs * 2 * 4, sizeof(*dec->nz_chroma_ac));
   dec->mv_l0_frame = MALLOC(dec->num_mbs * 16 * 2 * sizeof(*dec->mv_l0_frame));
   dec->ref_l0_frame = MALLOC(dec->num_mbs * 16 * sizeof(*dec->ref_l0_frame));
   if (!dec->intra4x4_modes || !dec->nz_luma || !dec->nz_chroma_ac ||
       !dec->mv_l0_frame || !dec->ref_l0_frame) {
      vl_h264_mb_decoder_fini(dec);
      return false;
   }
   memset(dec->intra4x4_modes, -1, dec->num_mbs * 16 * sizeof(*dec->intra4x4_modes));
   return true;
}

void
vl_h264_mb_decoder_fini(struct vl_h264_mb_decoder *dec)
{
   if (!dec)
      return;
   FREE(dec->intra4x4_modes);
   dec->intra4x4_modes = NULL;
   FREE(dec->nz_luma);
   dec->nz_luma = NULL;
   FREE(dec->nz_chroma_ac);
   dec->nz_chroma_ac = NULL;
   FREE(dec->mv_l0_frame);
   dec->mv_l0_frame = NULL;
   FREE(dec->ref_l0_frame);
   dec->ref_l0_frame = NULL;
}

void
vl_h264_mb_decoder_begin_slice(struct vl_h264_mb_decoder *dec,
                               const struct vl_h264_slice_header *slice)
{
   dec->slice = slice;
   dec->qp_y = slice->slice_qp;
   dec->skip_run = -1;
   /* Every block starts with no list-0 reference; a decoded inter block sets it,
    * and the motion vector prediction treats ref -1 as a non-matching neighbor. */
   memset(dec->ref_l0_frame, -1, dec->num_mbs * 16 * sizeof(*dec->ref_l0_frame));
}

bool
vl_h264_decode_intra_mb_body(struct vl_h264_mb_decoder *dec,
                             struct vl_h264_reader *reader, unsigned mb_x,
                             unsigned mb_y, struct vl_h264_mb_contract *mb,
                             unsigned mb_type)
{
   /* I_PCM and anything past it are out of scope; reject rather than misdecode.
    * The caller has already set the contract's position and slice type and, for
    * an intra macroblock inside a P slice, the intra reference markers. */
   if (mb_type >= I_PCM_MB_TYPE)
      return false;

   /* An intra macroblock has no reference; the marker lets the reconstruction
    * tell it from an inter macroblock in a P frame, and matches the I-slice
    * harnesses that do not otherwise set it. */
   for (int i = 0; i < 16; i++) {
      mb->ref_l0[i] = -1;
      mb->ref_l1[i] = -1;
   }

   bool has_cbp_field;
   if (mb_type == I_NXN_MB_TYPE) {
      mb->mb_type = I_NXN_MB_TYPE;
      decode_intra4x4_pred(dec, reader, mb_x, mb_y, mb);
      mb->cbp_luma = 0;
      mb->cbp_chroma = 0;
      has_cbp_field = true;
   } else {
      /* I_16x16 (Table 7-11): the prediction mode, cbp chroma, and cbp luma are
       * carried by the macroblock type itself. */
      unsigned n = mb_type - 1;
      mb->mb_type = mb_type;
      clear_intra4x4_modes(dec, mb_x, mb_y);
      if (chroma_array_type(dec->sps) == 1 || chroma_array_type(dec->sps) == 2)
         mb->intra_chroma_pred_mode = (uint8_t)vl_h264_ue(reader);
      mb->cbp_chroma = (n / 4) % 3;
      mb->cbp_luma = (n / 12) * 15;
      has_cbp_field = false;
   }

   if (has_cbp_field) {
      unsigned code_num = vl_h264_ue(reader);
      unsigned cbp = code_num < 48 ? intra_cbp_table[code_num] : 0;
      mb->cbp_luma = cbp & 0x0f;
      mb->cbp_chroma = cbp >> 4;
   }

   /* mb_qp_delta is present whenever any block is coded, and always for I_16x16
    * (sec 7.3.5).  QPY wraps modulo 52 for 8-bit (sec 7.4.5). */
   if (mb->cbp_luma || mb->cbp_chroma || mb_type != I_NXN_MB_TYPE) {
      int delta = vl_h264_se(reader);
      /* mb_qp_delta is constrained to [-26, 25] for 8-bit (sec 7.4.5); a value
       * outside that is a malformed stream and would drive QPY out of [0, 51],
       * past the dequant scaling tables.  Reject it. */
      if (delta < -26 || delta > 25)
         return false;
      dec->qp_y = (dec->qp_y + delta + 52) % 52;
   }

   mb->qp_y = dec->qp_y;
   mb->qp_cb = vl_h264_chroma_qp_from_luma(dec->qp_y, dec->pps->chroma_qp_index_offset);
   mb->qp_cr = vl_h264_chroma_qp_from_luma(dec->qp_y,
                                   dec->pps->second_chroma_qp_index_offset);
   /* Reject a header that ran past the end of the RBSP. */
   return !vl_h264_overrun(reader);
}

bool
vl_h264_decode_mb_header(struct vl_h264_mb_decoder *dec,
                         struct vl_h264_reader *reader, unsigned mb_x,
                         unsigned mb_y, struct vl_h264_mb_contract *mb)
{
   mb->mb_x = (int32_t)mb_x;
   mb->mb_y = (int32_t)mb_y;
   mb->slice_type = dec->slice->slice_type;
   mb->transform_8x8 = 0;

   /* The P-slice macroblock layer (skip run, inter types, intra-in-P) is owned
    * by vl_h264_decode_p_mb; this entry point handles an I slice. */
   if (dec->slice->slice_type != VL_H264_SLICE_I)
      return false;

   return vl_h264_decode_intra_mb_body(dec, reader, mb_x, mb_y, mb,
                                       vl_h264_ue(reader));
}
