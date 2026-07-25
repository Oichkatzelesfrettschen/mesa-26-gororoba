/*
 * SPDX-License-Identifier: MIT
 */

/*
 * libFuzzer harness for the H.264 P-slice macroblock layer.  The decoder parses
 * untrusted slice bytes, so it must reject any input cleanly -- no out-of-bounds
 * read, no overflow, no unbounded loop, no crash -- across the skip run, the
 * inter macroblock types and their motion vector prediction, and the intra
 * macroblocks a P slice carries.  Each input is one P-slice NAL decoded against
 * fixed Constrained-Baseline picture parameters.  Build with
 * -fsanitize=fuzzer,address,undefined or -fsanitize=fuzzer,memory.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_inter.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_slice_parser.h"

#define WIDTH_IN_MBS 11
#define HEIGHT_IN_MBS 9
#define NUM_MBS (WIDTH_IN_MBS * HEIGHT_IN_MBS)

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   if (size < 2)
      return 0;

   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct pipe_h264_picture_desc pic;

   memset(&sps, 0, sizeof(sps));
   sps.chroma_format_idc = 1;
   sps.pic_order_cnt_type = 2;
   sps.frame_mbs_only_flag = 1;
   sps.max_num_ref_frames = 1;
   sps.pic_width_in_mbs_minus1 = WIDTH_IN_MBS - 1;
   sps.pic_height_in_mbs_minus1 = HEIGHT_IN_MBS - 1;

   memset(&pps, 0, sizeof(pps));
   pps.sps = &sps;
   pps.pic_init_qp_minus26 = -3;
   pps.chroma_qp_index_offset = -2;
   pps.second_chroma_qp_index_offset = -2;
   pps.deblocking_filter_control_present_flag = 1;

   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   struct vl_h264_reader reader;
   if (!vl_h264_reader_init(&reader, data + 1, (unsigned)(size - 1)))
      return 0;

   struct vl_h264_slice_header sh;
   if (!vl_h264_parse_slice_header(&reader, &pic, (data[0] >> 5) & 3,
                                   data[0] & 0x1f, &sh)) {
      vl_h264_reader_fini(&reader);
      return 0;
   }

   struct vl_h264_mb_decoder dec;
   if (!vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS)) {
      vl_h264_reader_fini(&reader);
      return 0;
   }
   /* Force the P-slice path regardless of the fuzzed slice type. */
   sh.slice_type = VL_H264_SLICE_P;
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      struct vl_h264_mb_contract mb;
      struct vl_h264_mb_residual res;
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;

      enum vl_h264_p_mb_kind kind =
         vl_h264_decode_p_mb(&dec, &reader, mb_x, mb_y, &mb);
      if (kind == VL_H264_P_MB_ERROR)
         break;
      if (kind == VL_H264_P_MB_SKIP)
         continue;
      if (!vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mb, &res) ||
          !vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, &mb, &res))
         break;
   }

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   return 0;
}
