/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The clean-room Mesa-native CAVLC entropy provider: the active VL_H264_VLD
 * provider that turns a slice's raw NAL bytes directly into the per-macroblock
 * contract the GPU back half consumes.  It drives the front-end components --
 * the bitstream reader, the slice-header parse, the
 * macroblock-layer decode (intra and the inter motion vector prediction), the
 * CAVLC residual, and the dequantizer -- over the picture parameters the VA
 * frontend already populated, and fills out->macroblocks at each raster index.
 *
 * One slice is decoded against a fresh macroblock decoder: a Constrained
 * Baseline frame is a single slice covering the whole frame, and the
 * neighbor-availability rules make a macroblock in another slice unavailable
 * anyway, so per-slice decoder state is correct without carrying it across
 * slices.  Profiles out of scope -- anything but an I or P slice, or a feature
 * the front end rejects -- fail the slice rather than misdecode it.
 */

#include <stddef.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "util/u_memory.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_dequant.h"
#include "vl_h264_inter.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_slice_parser.h"
#include "vl_h264_vld_provider.h"

/* Offset of the NAL header byte past an Annex B start code (00 00 01 or
 * 00 00 00 01); the byte count when no start code is found, so the caller treats
 * the buffer as already pointing at the NAL header. */
static unsigned
annexb_header_offset(const uint8_t *nal, unsigned size)
{
   for (unsigned i = 0; i + 2 < size; i++)
      if (nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 1)
         return i + 3;
   return 0;
}

static bool
decode_one_macroblock(struct vl_h264_mb_decoder *dec,
                      struct vl_h264_reader *reader, bool p_slice,
                      unsigned mb_x, unsigned mb_y,
                      struct vl_h264_mb_contract *mb)
{
   struct vl_h264_mb_residual res;
   bool coded = true;

   if (p_slice) {
      enum vl_h264_p_mb_kind kind =
         vl_h264_decode_p_mb(dec, reader, mb_x, mb_y, mb);
      if (kind == VL_H264_P_MB_ERROR)
         return false;
      coded = kind != VL_H264_P_MB_SKIP;
   } else if (!vl_h264_decode_mb_header(dec, reader, mb_x, mb_y, mb)) {
      return false;
   }

   if (coded) {
      if (!vl_h264_decode_mb_luma_residual(dec, reader, mb_x, mb_y, mb, &res) ||
          !vl_h264_decode_mb_chroma_residual(dec, reader, mb_x, mb_y, mb, &res))
         return false;
      vl_h264_dequant_fill_contract(&res, mb);
   } else {
      memset(mb->coeff4x4, 0, sizeof(mb->coeff4x4));
   }

   /* The in-loop deblock reads disable_deblock_idc and the alpha/beta offsets per
    * macroblock to index the filter thresholds (sec 8.7.2.2).  The slice parser
    * decodes them into the slice header, but only the macroblock contract reaches
    * the deblock, so without this copy every macroblock carried offset zero and a
    * stream signalling non-default slice_alpha_c0_offset/slice_beta_offset was
    * filtered at the wrong QP index. */
   mb->disable_deblock_idc = (int8_t) dec->slice->disable_deblocking_filter_idc;
   mb->slice_alpha_c0_offset_div2 =
      (int8_t) dec->slice->slice_alpha_c0_offset_div2;
   mb->slice_beta_offset_div2 = (int8_t) dec->slice->slice_beta_offset_div2;
   return true;
}

static bool
vl_h264_cavlc_decode_slice(struct vl_h264_vld_provider *provider,
                           const struct pipe_h264_picture_desc *picture,
                           const uint8_t *nal, unsigned nal_size,
                           struct vl_h264_slice_contract *out)
{
   (void) provider;

   if (!picture || !picture->pps || !picture->pps->sps || !nal)
      return false;
   const struct pipe_h264_sps *sps = picture->pps->sps;

   unsigned off = annexb_header_offset(nal, nal_size);
   if (off >= nal_size)
      return false;
   uint8_t header = nal[off];
   unsigned nal_ref_idc = (header >> 5) & 3;
   unsigned nal_unit_type = header & 0x1f;
   /* Only coded slice NAL units (type 1 and 5) carry macroblocks. */
   if (nal_unit_type != 1 && nal_unit_type != 5)
      return true;

   struct vl_h264_reader reader;
   if (!vl_h264_reader_init(&reader, nal + off + 1, nal_size - off - 1))
      return false;

   struct vl_h264_slice_header sh;
   if (!vl_h264_parse_slice_header(&reader, picture, nal_ref_idc, nal_unit_type,
                                   &sh)) {
      vl_h264_reader_fini(&reader);
      return false;
   }
   /* Constrained Baseline is I and P only; reject B/SP/SI rather than misdecode. */
   if (sh.slice_type != VL_H264_SLICE_I && sh.slice_type != VL_H264_SLICE_P) {
      vl_h264_reader_fini(&reader);
      return false;
   }

   unsigned width_in_mbs = sps->pic_width_in_mbs_minus1 + 1;
   unsigned height_in_mbs = sps->pic_height_in_mbs_minus1 + 1;
   unsigned num_mbs = width_in_mbs * height_in_mbs;

   /* A slice header that ran past the end of the NAL, or whose first macroblock
    * lies outside the frame, is malformed; fail rather than skip the slice with a
    * silent success. */
   if (vl_h264_overrun(&reader) || sh.first_mb_in_slice >= num_mbs) {
      vl_h264_reader_fini(&reader);
      return false;
   }

   struct vl_h264_mb_decoder dec;
   if (!vl_h264_mb_decoder_init(&dec, picture, width_in_mbs, height_in_mbs)) {
      vl_h264_reader_fini(&reader);
      return false;
   }
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   /* Decode this slice's macroblocks from first_mb_in_slice onward.  num_mbs
    * bounds the loop and an end-of-bitstream overrun stops it at the slice's
    * last macroblock, so a frame split into several slices is decoded one
    * decode_slice call per slice.  A P slice can end with one mb_skip_run that
    * infers the trailing skipped macroblocks, so more_rbsp_data
    * turns false while skip macroblocks remain to emit and the loop must run
    * past it. */
   bool p_slice = sh.slice_type == VL_H264_SLICE_P;
   bool ok = true;
   for (unsigned addr = sh.first_mb_in_slice; addr < num_mbs; addr++) {
      if (addr >= out->num_macroblocks) {
         ok = false;
         break;
      }
      if (!decode_one_macroblock(&dec, &reader, p_slice, addr % width_in_mbs,
                                 addr / width_in_mbs, &out->macroblocks[addr])) {
         ok = false;
         break;
      }
      /* Record this macroblock's slice so intra prediction can reject a
       * neighbor across the slice boundary. */
      out->macroblocks[addr].slice_first_mb = (int32_t) sh.first_mb_in_slice;
   }

   if (ok) {
      out->slice_type = sh.slice_type;
      /* Carry this slice's RefPicList0 reordering to end_frame.  A Constrained
       * Baseline frame is a single slice, so the slice's commands are the
       * frame's; build_ref_pic_list0 reconstructs the reordered list. */
      out->num_reorder_l0 = sh.num_reorder_l0;
      memcpy(out->reorder_l0, sh.reorder_l0,
             sh.num_reorder_l0 * sizeof(sh.reorder_l0[0]));
   }
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   return ok;
}

static void
vl_h264_cavlc_destroy(struct vl_h264_vld_provider *provider)
{
   FREE(provider);
}

struct vl_h264_vld_provider *
vl_h264_cavlc_provider_create(void)
{
   struct vl_h264_vld_provider *provider = CALLOC_STRUCT(vl_h264_vld_provider);
   if (!provider)
      return NULL;
   provider->kind = VL_H264_VLD_PROVIDER_MESA_CAVLC;
   provider->priv = NULL;
   provider->decode_slice = vl_h264_cavlc_decode_slice;
   provider->destroy = vl_h264_cavlc_destroy;
   return provider;
}
