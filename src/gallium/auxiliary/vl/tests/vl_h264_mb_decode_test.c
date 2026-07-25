/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Stage test for the macroblock-layer decoder: parse the slice header of a real
 * Constrained-Baseline IDR slice, then decode the header of its first
 * macroblock and assert the macroblock type, QP, and coded block pattern against
 * the libavcodec oracle dump for that frame.  The macroblock header ends at
 * mb_qp_delta, before the residual, so this validates the C3 output without the
 * residual stage: oracle record 0 is Intra_4x4 at qscale 27 with luma coded in
 * all four 8x8 quadrants, so cbp_luma must be 0x0f.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_mb_decode.h"
#include "vl_h264_slice_parser.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

static void
fill_fixture_sps_pps(struct pipe_h264_sps *sps, struct pipe_h264_pps *pps)
{
   memset(sps, 0, sizeof(*sps));
   sps->chroma_format_idc = 1;
   sps->log2_max_frame_num_minus4 = 0;
   sps->pic_order_cnt_type = 2;
   sps->frame_mbs_only_flag = 1;
   sps->max_num_ref_frames = 1;
   sps->pic_width_in_mbs_minus1 = 10;
   sps->pic_height_in_mbs_minus1 = 8;

   memset(pps, 0, sizeof(*pps));
   pps->sps = sps;
   pps->entropy_coding_mode_flag = 0;
   pps->num_ref_idx_l0_default_active_minus1 = 0;
   pps->pic_init_qp_minus26 = -3;
   pps->deblocking_filter_control_present_flag = 1;
}

int
main(int argc, char **argv)
{
   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct pipe_h264_picture_desc pic;
   struct vl_h264_slice_header sh;
   struct vl_h264_reader reader;
   struct vl_h264_mb_decoder dec;
   struct vl_h264_mb_contract mb;
   uint8_t *nal;
   long nal_size;
   FILE *f;

   CHECK(argc > 1);
   f = fopen(argv[1], "rb");
   CHECK(f != NULL);
   fseek(f, 0, SEEK_END);
   nal_size = ftell(f);
   fseek(f, 0, SEEK_SET);
   CHECK(nal_size > 1);
   nal = malloc(nal_size);
   CHECK(nal != NULL);
   CHECK(fread(nal, 1, nal_size, f) == (size_t)nal_size);
   fclose(f);

   unsigned nal_ref_idc = (nal[0] >> 5) & 3;
   unsigned nal_unit_type = nal[0] & 0x1f;

   fill_fixture_sps_pps(&sps, &pps);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, nal_ref_idc, nal_unit_type,
                                    &sh));
   CHECK(sh.slice_type == VL_H264_SLICE_I);

   CHECK(vl_h264_mb_decoder_init(&dec, &pic, 11, 9));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   memset(&mb, 0, sizeof(mb));
   CHECK(vl_h264_decode_mb_header(&dec, &reader, 0, 0, &mb));

   /* Oracle record 0: Intra_4x4 (mb_type 0 = I_NxN), qscale 27, luma coded in
    * every 8x8 quadrant. */
   CHECK(mb.mb_type == 0);
   CHECK(mb.slice_type == VL_H264_SLICE_I);
   CHECK(mb.qp_y == 27);
   CHECK(mb.cbp_luma == 0x0f);
   CHECK(mb.intra_chroma_pred_mode <= 3);
   for (unsigned i = 0; i < 16; i++)
      CHECK(mb.intra4x4_pred_mode[i] <= 8);

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(nal);
   printf("vl_h264_mb_decode: IDR MB0 header matches oracle (I_NxN, qp 27, "
          "cbp_luma 0x0f) PASS\n");
   return 0;
}
