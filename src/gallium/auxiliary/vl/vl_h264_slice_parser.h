/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Clean-room H.264 slice-header parser for the Mesa-native CAVLC front end.
 *
 * The VA frontend supplies the active SPS and PPS in pipe_h264_picture_desc and
 * hands the provider the slice's raw NAL.  This parser consumes the slice header
 * (ITU-T H.264 sec 7.3.3) from the NAL's RBSP, using the SPS/PPS for the
 * variable field widths and conditionals, and produces the per-slice state the
 * macroblock loop needs: slice type, the running QP base, the deblock override,
 * and the active reference count.  Scope is Constrained Baseline: I and P slices,
 * frame_mbs_only, CAVLC; the picture-order-count and reference-list-modification
 * and marking syntax are parsed in full so the bit position stays aligned even
 * where the field is not retained.
 */

#ifndef vl_h264_slice_parser_h
#define vl_h264_slice_parser_h

#include <stdbool.h>

#include "vl_h264_bitstream.h"
#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_h264_picture_desc;

struct vl_h264_slice_header {
   enum vl_h264_slice_type slice_type;  /* normalized to 0..4 (slice_type % 5) */
   bool idr;                            /* nal_unit_type == 5 */
   unsigned nal_ref_idc;
   unsigned first_mb_in_slice;
   unsigned pic_parameter_set_id;
   unsigned frame_num;
   unsigned idr_pic_id;                 /* meaningful only when idr */
   int slice_qp;                        /* SliceQPY = 26 + pic_init_qp_minus26 +
                                         * slice_qp_delta (sec 7.4.3) */
   unsigned num_ref_idx_l0_active;      /* effective list-0 reference count */
   int disable_deblocking_filter_idc;   /* 0 when deblock control is absent */
   int slice_alpha_c0_offset_div2;
   int slice_beta_offset_div2;

   /* ref_pic_list_modification commands for list 0 (sec 7.3.3.1); zero when
    * ref_pic_list_modification_flag_l0 is 0. */
   uint32_t num_reorder_l0;
   struct vl_h264_ref_reorder reorder_l0[VL_H264_MAX_REORDER_L0];
};

/*
 * Parse one slice header from reader, which must be positioned at the first
 * slice-header bit (the provider has stripped the start code and the one-byte
 * NAL header and passes its nal_ref_idc/nal_unit_type here).  picture supplies
 * the active PPS and SPS.  Returns false when the PPS/SPS are missing or the
 * slice type is outside the Constrained Baseline subset.
 */
bool vl_h264_parse_slice_header(struct vl_h264_reader *reader,
                                const struct pipe_h264_picture_desc *picture,
                                unsigned nal_ref_idc, unsigned nal_unit_type,
                                struct vl_h264_slice_header *out);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_slice_parser_h */
