/*
 * SPDX-License-Identifier: MIT
 */

/*
 * libFuzzer harness for the Mesa-native H.264 CAVLC front end.  The decoder
 * parses untrusted slice bytes, so it must reject any input cleanly -- no
 * out-of-bounds read, no overflow, no unbounded loop, no crash.  Each fuzz input
 * is one slice NAL (a one-byte header then the EBSP) decoded against fixed
 * Constrained-Baseline picture parameters: the bitstream reader, the slice
 * header, every macroblock's header and residual, the dequant, and the intra
 * reconstruction.  Build with -fsanitize=fuzzer,address,undefined.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_dequant.h"
#include "vl_h264_intra_reconstruct.h"
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
   sps.frame_mbs_only_flag = 1;
   sps.pic_order_cnt_type = 2;
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
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   struct vl_h264_mb_contract *mbs = calloc(NUM_MBS, sizeof(*mbs));
   if (mbs) {
      unsigned decoded = 0;
      for (unsigned a = 0; a < NUM_MBS; a++) {
         unsigned mb_x = a % WIDTH_IN_MBS, mb_y = a / WIDTH_IN_MBS;
         struct vl_h264_mb_residual res;
         if (!vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, &mbs[a]) ||
             !vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mbs[a],
                                              &res) ||
             !vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y,
                                                &mbs[a], &res))
            break;
         vl_h264_dequant_fill_contract(&res, &mbs[a]);
         decoded++;
      }

      /* A fully decoded frame exercises the reconstruction on fuzzed contracts. */
      if (decoded == NUM_MBS) {
         uint8_t *luma = calloc(WIDTH_IN_MBS * 16, HEIGHT_IN_MBS * 16);
         uint8_t *cb = calloc(WIDTH_IN_MBS * 8, HEIGHT_IN_MBS * 8);
         uint8_t *cr = calloc(WIDTH_IN_MBS * 8, HEIGHT_IN_MBS * 8);
         if (luma && cb && cr) {
            vl_h264_intra_reconstruct_luma(mbs, NUM_MBS, WIDTH_IN_MBS,
                                           HEIGHT_IN_MBS, luma, WIDTH_IN_MBS * 16,
                                           false);
            vl_h264_intra_reconstruct_chroma(mbs, NUM_MBS, WIDTH_IN_MBS,
                                             HEIGHT_IN_MBS, cb, cr,
                                             WIDTH_IN_MBS * 8, false);
         }
         free(luma);
         free(cb);
         free(cr);
      }
      free(mbs);
   }

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   return 0;
}
