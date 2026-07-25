/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Real-bitstream test for the luma residual traversal: parse the IDR slice
 * header and the first macroblock header, then decode its luma residual and check
 * the sixteen blocks' TotalCoeff against the libavcodec oracle for that frame.
 *
 * The check is on the sorted multiset of per-block counts and their total, not
 * per-index, so it does not depend on the exact block-order mapping between the
 * decoder and the oracle dump (the later dequant gate pins that).  It is still a
 * strong nC check: a wrong neighbour context selects the wrong coeff_token table
 * and changes the counts.  Oracle record 0 of the QCIF fixture has block counts
 * {0,0,0,0,1,1,8,11,11,11,11,12,13,15,15,15}, totalling 124 nonzero coefficients.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
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

static int
compare_uint8(const void *a, const void *b)
{
   return (int)*(const uint8_t *)a - (int)*(const uint8_t *)b;
}

int
main(int argc, char **argv)
{
   static const uint8_t oracle_sorted[16] =
      { 0, 0, 0, 0, 1, 1, 8, 11, 11, 11, 11, 12, 13, 15, 15, 15 };

   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct pipe_h264_picture_desc pic;
   struct vl_h264_slice_header sh;
   struct vl_h264_reader reader;
   struct vl_h264_mb_decoder dec;
   struct vl_h264_mb_contract mb;
   struct vl_h264_mb_residual res;
   uint8_t *nal, sorted[16];
   long nal_size;
   unsigned total = 0;
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
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, 11, 9));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   memset(&mb, 0, sizeof(mb));
   CHECK(vl_h264_decode_mb_header(&dec, &reader, 0, 0, &mb));
   CHECK(mb.cbp_luma == 0x0f);
   CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, 0, 0, &mb, &res));

   memcpy(sorted, res.nz_luma, sizeof(sorted));
   qsort(sorted, 16, 1, compare_uint8);
   for (unsigned i = 0; i < 16; i++) {
      CHECK(sorted[i] == oracle_sorted[i]);
      total += res.nz_luma[i];
   }
   CHECK(total == 124);

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(nal);
   printf("vl_h264_cavlc_residual: MB0 luma block counts match the oracle "
          "(124 coeffs, nC traversal) PASS\n");
   return 0;
}
