/*
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_slice_parser.h"

/*
 * Clean-room from ITU-T H.264 sec 7.3.3 and its sub-clauses; no third-party
 * decoder source was consulted.  The sub-syntax that Constrained Baseline never
 * exercises (reference-list modification, prediction weights, adaptive marking)
 * is still consumed so the reader stays bit-aligned for the macroblock loop.
 */

#define VL_H264_NAL_UNIT_TYPE_IDR_SLICE 5

/* ChromaArrayType (sec 7.4.2.1.1): 0 when colour planes are coded separately,
 * otherwise the chroma_format_idc. */
static unsigned
chroma_array_type(const struct pipe_h264_sps *sps)
{
   return sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
}

/* ref_pic_list_modification (sec 7.3.3.1): parse the list-0 (and, for B, the
 * list-1) reordering commands.  The list-0 commands are retained in out so
 * build_ref_pic_list0 can reorder RefPicList0 (sec 8.2.4.3.1); list-1 (B only) is
 * consumed without retention. */
static bool
parse_ref_pic_list_modification(struct vl_h264_reader *reader,
                                struct vl_h264_slice_header *out)
{
   enum vl_h264_slice_type slice_type = out->slice_type;
   unsigned lists = (slice_type == VL_H264_SLICE_B) ? 2 : 1;

   out->num_reorder_l0 = 0;
   if (slice_type == VL_H264_SLICE_I || slice_type == VL_H264_SLICE_SI)
      return true;

   for (unsigned list = 0; list < lists; list++) {
      if (list == 1 && slice_type != VL_H264_SLICE_B)
         break;
      if (!vl_h264_u(reader, 1)) { /* ref_pic_list_modification_flag_lX */
         if (vl_h264_overrun(reader))
            return false;
         continue;
      }
      unsigned idc;
      do {
         if (!vl_h264_more_rbsp_data(reader))
            return false;

         idc = vl_h264_ue(reader);
         if (vl_h264_overrun(reader) || idc > 3)
            return false;

         unsigned value = 0;
         if (idc == 0 || idc == 1 || idc == 2)
            value = vl_h264_ue(reader); /* abs_diff_pic_num_minus1 / long_term_pic_num */
         if (vl_h264_overrun(reader))
            return false;

         if (list == 0 && idc != 3 &&
             out->num_reorder_l0 < VL_H264_MAX_REORDER_L0) {
            out->reorder_l0[out->num_reorder_l0].idc = (uint8_t) idc;
            out->reorder_l0[out->num_reorder_l0].value = value;
            out->num_reorder_l0++;
         }
      } while (idc != 3);
   }

   return true;
}

/* pred_weight_table (sec 7.3.3.2): consume the explicit weights.  Constrained
 * Baseline has weighted_pred_flag == 0, so this never fires, but it is parsed
 * for correctness on any conforming P/B slice that sets it. */
static void
parse_pred_weight_table(struct vl_h264_reader *reader,
                        const struct pipe_h264_sps *sps,
                        enum vl_h264_slice_type slice_type,
                        unsigned num_ref_idx_l0_active)
{
   bool has_chroma = chroma_array_type(sps) != 0;
   unsigned lists = (slice_type == VL_H264_SLICE_B) ? 2 : 1;

   (void) vl_h264_ue(reader);                 /* luma_log2_weight_denom */
   if (has_chroma)
      (void) vl_h264_ue(reader);              /* chroma_log2_weight_denom */

   for (unsigned list = 0; list < lists; list++) {
      for (unsigned i = 0; i < num_ref_idx_l0_active; i++) {
         if (vl_h264_u(reader, 1)) {          /* luma_weight_lX_flag */
            (void) vl_h264_se(reader);        /* luma_weight_lX */
            (void) vl_h264_se(reader);        /* luma_offset_lX */
         }
         if (has_chroma && vl_h264_u(reader, 1)) {  /* chroma_weight_lX_flag */
            for (unsigned j = 0; j < 2; j++) {
               (void) vl_h264_se(reader);     /* chroma_weight_lX */
               (void) vl_h264_se(reader);     /* chroma_offset_lX */
            }
         }
      }
   }
}

/* dec_ref_pic_marking (sec 7.3.3.3): consume the marking commands. */
static void
parse_dec_ref_pic_marking(struct vl_h264_reader *reader, bool idr)
{
   if (idr) {
      (void) vl_h264_u(reader, 1);   /* no_output_of_prior_pics_flag */
      (void) vl_h264_u(reader, 1);   /* long_term_reference_flag */
      return;
   }

   if (!vl_h264_u(reader, 1))        /* adaptive_ref_pic_marking_mode_flag */
      return;

   unsigned mmco;
   do {
      mmco = vl_h264_ue(reader);
      if (mmco == 1 || mmco == 3)
         (void) vl_h264_ue(reader);  /* difference_of_pic_nums_minus1 */
      if (mmco == 2)
         (void) vl_h264_ue(reader);  /* long_term_pic_num */
      if (mmco == 3 || mmco == 6)
         (void) vl_h264_ue(reader);  /* long_term_frame_idx */
      if (mmco == 4)
         (void) vl_h264_ue(reader);  /* max_long_term_frame_idx_plus1 */
   } while (mmco != 0);
}

/* Picture order count (sec 7.3.3): consumed to keep the reader aligned; a
 * single-reference CB decode does not use POC. */
static void
parse_pic_order_cnt(struct vl_h264_reader *reader,
                    const struct pipe_h264_sps *sps,
                    const struct pipe_h264_pps *pps, unsigned field_pic_flag)
{
   bool both_fields = pps->bottom_field_pic_order_in_frame_present_flag &&
                      !field_pic_flag;

   if (sps->pic_order_cnt_type == 0) {
      (void) vl_h264_u(reader, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
      if (both_fields)
         (void) vl_h264_se(reader);     /* delta_pic_order_cnt_bottom */
   } else if (sps->pic_order_cnt_type == 1 &&
              !sps->delta_pic_order_always_zero_flag) {
      (void) vl_h264_se(reader);        /* delta_pic_order_cnt[0] */
      if (both_fields)
         (void) vl_h264_se(reader);     /* delta_pic_order_cnt[1] */
   }
}

/* Deblock filter control (sec 7.3.3): the idc and, unless filtering is off, the
 * alpha/beta offsets the back half needs. */
static void
parse_deblock_control(struct vl_h264_reader *reader,
                      const struct pipe_h264_pps *pps,
                      struct vl_h264_slice_header *out)
{
   if (!pps->deblocking_filter_control_present_flag)
      return;

   out->disable_deblocking_filter_idc = vl_h264_ue(reader);
   if (out->disable_deblocking_filter_idc != 1) {
      out->slice_alpha_c0_offset_div2 = vl_h264_se(reader);
      out->slice_beta_offset_div2 = vl_h264_se(reader);
   }
}

bool
vl_h264_parse_slice_header(struct vl_h264_reader *reader,
                           const struct pipe_h264_picture_desc *picture,
                           unsigned nal_ref_idc, unsigned nal_unit_type,
                           struct vl_h264_slice_header *out)
{
   const struct pipe_h264_pps *pps = picture ? picture->pps : NULL;
   const struct pipe_h264_sps *sps = pps ? pps->sps : NULL;
   if (!pps || !sps)
      return false;

   /* ITU-T H.264 sec 7.4.2.1.1 bounds the SPS fields that determine
    * slice-header u(v) widths and picture-order syntax. */
   if (sps->log2_max_frame_num_minus4 > 12 ||
       sps->pic_order_cnt_type > 2 ||
       (sps->pic_order_cnt_type == 0 &&
        sps->log2_max_pic_order_cnt_lsb_minus4 > 12))
      return false;

   if (pps->entropy_coding_mode_flag)
      return false;

   memset(out, 0, sizeof(*out));
   out->nal_ref_idc = nal_ref_idc;
   out->idr = (nal_unit_type == VL_H264_NAL_UNIT_TYPE_IDR_SLICE);

   out->first_mb_in_slice = vl_h264_ue(reader);

   unsigned slice_type_raw = vl_h264_ue(reader);
   out->slice_type = (enum vl_h264_slice_type)(slice_type_raw % 5);
   /* Constrained Baseline carries only I and P slices. */
   if (out->slice_type != VL_H264_SLICE_I && out->slice_type != VL_H264_SLICE_P)
      return false;

   out->pic_parameter_set_id = vl_h264_ue(reader);

   /* colour_plane_id only exists with separate colour planes, which CB lacks. */
   out->frame_num = vl_h264_u(reader, sps->log2_max_frame_num_minus4 + 4);

   /* Frame-raster reconstruction cannot represent field macroblock addresses.
    * Consume bottom_field_flag for field pictures, then reject MBAFF frames
    * before any syntax consumer can interpret field macroblock state. */
   unsigned field_pic_flag = 0;
   if (!sps->frame_mbs_only_flag) {
      field_pic_flag = vl_h264_u(reader, 1);
      if (field_pic_flag) {
         (void) vl_h264_u(reader, 1);   /* bottom_field_flag */
         return false;
      }
      if (sps->mb_adaptive_frame_field_flag)
         return false;
   }

   if (out->idr)
      out->idr_pic_id = vl_h264_ue(reader);

   parse_pic_order_cnt(reader, sps, pps, field_pic_flag);

   if (pps->redundant_pic_cnt_present_flag)
      (void) vl_h264_ue(reader);        /* redundant_pic_cnt */

   /* The default active count is the slice-header fallback when
    * num_ref_idx_active_override_flag is 0.  VAPictureParameterBufferH264 carries
    * no num_ref_idx_l0_default_active_minus1, so pps->num_ref_idx_l0_default_active_minus1
    * is always zero here; the VA frontend instead reports the effective per-slice
    * count in num_ref_idx_l0_active_minus1 (VASliceParameterBufferH264), which
    * already reflects any override.  Seed the default from it so a slice that omits
    * the override decodes ref_idx_l0 with the correct active count. */
   out->num_ref_idx_l0_active = picture->num_ref_idx_l0_active_minus1 + 1;
   if (out->slice_type == VL_H264_SLICE_P) {
      if (vl_h264_u(reader, 1))         /* num_ref_idx_active_override_flag */
         out->num_ref_idx_l0_active = vl_h264_ue(reader) + 1;
   }

   if (!parse_ref_pic_list_modification(reader, out))
      return false;

   if (pps->weighted_pred_flag && out->slice_type == VL_H264_SLICE_P)
      parse_pred_weight_table(reader, sps, out->slice_type,
                              out->num_ref_idx_l0_active);

   if (nal_ref_idc != 0)
      parse_dec_ref_pic_marking(reader, out->idr);

   /* entropy_coding_mode_flag is 0 (CAVLC) for CB, so cabac_init_idc is absent. */
   int slice_qp_delta = vl_h264_se(reader);
   out->slice_qp = 26 + pps->pic_init_qp_minus26 + slice_qp_delta;

   parse_deblock_control(reader, pps, out);

   return true;
}
