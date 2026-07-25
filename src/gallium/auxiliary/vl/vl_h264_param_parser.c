/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "vl_h264_param_parser.h"

/*
 * Clean-room from ITU-T H.264 sec 7.3.2.1 (sequence parameter set) and 7.3.2.2
 * (picture parameter set).  The high-profile chroma and bit-depth block is
 * present only for the High-profile family; a Baseline stream skips it and the
 * inferred defaults apply (4:2:0, 8-bit).
 */

/* The profile_idc values that carry the high-profile chroma_format_idc /
 * bit_depth / scaling-list block (sec 7.3.2.1). */
static bool
is_high_profile(unsigned profile_idc)
{
   switch (profile_idc) {
   case 100: case 110: case 122: case 244: case 44:
   case 83: case 86: case 118: case 128: case 138: case 139: case 134:
      return true;
   default:
      return false;
   }
}

bool
vl_h264_parse_sps(struct vl_h264_reader *reader, struct pipe_h264_sps *sps)
{
   memset(sps, 0, sizeof(*sps));

   unsigned profile_idc = vl_h264_u(reader, 8);
   vl_h264_u(reader, 8); /* constraint_set flags and reserved bits */
   sps->level_idc = (uint8_t)vl_h264_u(reader, 8);
   vl_h264_ue(reader); /* seq_parameter_set_id */

   /* 4:2:0 8-bit are the inferred defaults a Baseline stream relies on. */
   sps->chroma_format_idc = 1;
   if (is_high_profile(profile_idc)) {
      sps->chroma_format_idc = (uint8_t)vl_h264_ue(reader);
      if (sps->chroma_format_idc == 3)
         sps->separate_colour_plane_flag = (uint8_t)vl_h264_u(reader, 1);
      sps->bit_depth_luma_minus8 = (uint8_t)vl_h264_ue(reader);
      sps->bit_depth_chroma_minus8 = (uint8_t)vl_h264_ue(reader);
      vl_h264_u(reader, 1); /* qpprime_y_zero_transform_bypass_flag */
      if (vl_h264_u(reader, 1)) /* seq_scaling_matrix_present_flag */
         return false;         /* scaling lists are out of scope */
   }

   sps->log2_max_frame_num_minus4 = (uint8_t)vl_h264_ue(reader);
   sps->pic_order_cnt_type = (uint8_t)vl_h264_ue(reader);
   if (sps->pic_order_cnt_type == 0) {
      sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)vl_h264_ue(reader);
   } else if (sps->pic_order_cnt_type == 1) {
      sps->delta_pic_order_always_zero_flag = (uint8_t)vl_h264_u(reader, 1);
      sps->offset_for_non_ref_pic = vl_h264_se(reader);
      sps->offset_for_top_to_bottom_field = vl_h264_se(reader);
      unsigned n = vl_h264_ue(reader);
      if (n > 255)
         return false;
      sps->num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)n;
      for (unsigned i = 0; i < n; i++)
         sps->offset_for_ref_frame[i] = vl_h264_se(reader);
   }

   sps->max_num_ref_frames = (uint8_t)vl_h264_ue(reader);
   sps->gaps_in_frame_num_value_allowed_flag = (uint8_t)vl_h264_u(reader, 1);
   sps->pic_width_in_mbs_minus1 = vl_h264_ue(reader);
   unsigned height_map_units_minus1 = vl_h264_ue(reader);
   sps->frame_mbs_only_flag = (uint8_t)vl_h264_u(reader, 1);
   if (!sps->frame_mbs_only_flag)
      sps->mb_adaptive_frame_field_flag = (uint8_t)vl_h264_u(reader, 1);

   /* Frame height in macroblocks: a field picture map unit is half a frame
    * macroblock (sec 7.4.2.1.1). */
   sps->pic_height_in_mbs_minus1 =
      (height_map_units_minus1 + 1) * (2 - sps->frame_mbs_only_flag) - 1;

   sps->direct_8x8_inference_flag = (uint8_t)vl_h264_u(reader, 1);
   if (vl_h264_u(reader, 1)) { /* frame_cropping_flag */
      vl_h264_ue(reader);
      vl_h264_ue(reader);
      vl_h264_ue(reader);
      vl_h264_ue(reader);
   }
   /* vui_parameters_present_flag and the VUI itself are not needed for decode. */

   return !vl_h264_overrun(reader);
}

bool
vl_h264_parse_pps(struct vl_h264_reader *reader, struct pipe_h264_sps *sps,
                  struct pipe_h264_pps *pps)
{
   memset(pps, 0, sizeof(*pps));
   pps->sps = sps;

   vl_h264_ue(reader); /* pic_parameter_set_id */
   vl_h264_ue(reader); /* seq_parameter_set_id */
   pps->entropy_coding_mode_flag = (uint8_t)vl_h264_u(reader, 1);
   pps->bottom_field_pic_order_in_frame_present_flag =
      (uint8_t)vl_h264_u(reader, 1);

   pps->num_slice_groups_minus1 = (uint8_t)vl_h264_ue(reader);
   if (pps->num_slice_groups_minus1 > 0)
      return false; /* slice groups are out of scope */

   pps->num_ref_idx_l0_default_active_minus1 = (uint8_t)vl_h264_ue(reader);
   pps->num_ref_idx_l1_default_active_minus1 = (uint8_t)vl_h264_ue(reader);
   pps->weighted_pred_flag = (uint8_t)vl_h264_u(reader, 1);
   pps->weighted_bipred_idc = (uint8_t)vl_h264_u(reader, 2);
   pps->pic_init_qp_minus26 = (int8_t)vl_h264_se(reader);
   pps->pic_init_qs_minus26 = (int8_t)vl_h264_se(reader);
   pps->chroma_qp_index_offset = (int8_t)vl_h264_se(reader);
   pps->deblocking_filter_control_present_flag = (uint8_t)vl_h264_u(reader, 1);
   pps->constrained_intra_pred_flag = (uint8_t)vl_h264_u(reader, 1);
   pps->redundant_pic_cnt_present_flag = (uint8_t)vl_h264_u(reader, 1);

   /* The optional extension carries the 8x8 transform flag, scaling lists, and a
    * second chroma QP offset; without it the second offset equals the first
    * (sec 7.4.2.2). */
   if (vl_h264_more_rbsp_data(reader)) {
      pps->transform_8x8_mode_flag = (uint8_t)vl_h264_u(reader, 1);
      if (vl_h264_u(reader, 1)) /* pic_scaling_matrix_present_flag */
         return false;          /* scaling lists are out of scope */
      pps->second_chroma_qp_index_offset = (int8_t)vl_h264_se(reader);
   } else {
      pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
   }

   return !vl_h264_overrun(reader);
}
