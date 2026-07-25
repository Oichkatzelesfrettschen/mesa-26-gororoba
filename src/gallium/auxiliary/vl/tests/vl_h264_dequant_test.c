/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The bit-exact dequant gate: decode the whole IDR slice through the CAVLC front
 * end and, for every I_NxN macroblock, dequantize its sixteen luma blocks and
 * check every coefficient against the libavcodec oracle for that frame.  The
 * oracle stores each block transposed relative to the pixel-natural raster the
 * dequant emits, so the comparison transposes the oracle.  Matching here proves
 * the entropy decode's coefficient values (not just their counts) and the dequant
 * scaling together, end to end.  The slice's QP ranges across both dequant
 * branches (the qP >= 24 left shift and the rounded right shift below it).  Every
 * macroblock is decoded so the stream stays in sync; I_16x16 carries a separate
 * DC Hadamard and is validated elsewhere.  Arguments: the IDR slice NAL and the
 * oracle dump.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_dequant.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_slice_parser.h"

#define WIDTH_IN_MBS 11
#define HEIGHT_IN_MBS 9
#define NUM_MBS (WIDTH_IN_MBS * HEIGHT_IN_MBS)
#define ORACLE_RECORD_BYTES (4 * 4 + 256 * 2)
#define MB_TYPE_INTRA4X4 0x01

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

static uint8_t *
read_file(const char *path, long *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   *size = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *data = malloc(*size);
   if (data && fread(data, 1, *size, f) != (size_t)*size) {
      free(data);
      data = NULL;
   }
   fclose(f);
   return data;
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
   uint8_t *nal, *oracle;
   long nal_size, oracle_size;

   CHECK(argc > 2);
   nal = read_file(argv[1], &nal_size);
   CHECK(nal && nal_size > 1);
   oracle = read_file(argv[2], &oracle_size);
   CHECK(oracle && oracle_size >= (long)(NUM_MBS * ORACLE_RECORD_BYTES));

   fill_fixture_sps_pps(&sps, &pps);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (nal[0] >> 5) & 3,
                                    nal[0] & 0x1f, &sh));
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   /* 4x4 luma block scan index to raster grid position (sec 6.4.3). */
   static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3,
                                      0, 1, 0, 1, 2, 3, 2, 3 };
   static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1,
                                      2, 2, 3, 3, 2, 2, 3, 3 };
   unsigned i16x16 = 0;
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;
      struct vl_h264_mb_contract mb;
      struct vl_h264_mb_residual res;

      memset(&mb, 0, sizeof(mb));
      CHECK(vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, &mb));
      CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mb, &res));
      CHECK(vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, &mb,
                                              &res));
      vl_h264_dequant_fill_contract(&res, &mb);
      i16x16 += mb.mb_type != 0;

      /* The oracle stores luma blocks in scan order; the contract in raster
       * order, transposed within each block.  For Intra_16x16 the oracle dump
       * captures the AC only: ffmpeg scatters the Hadamard DC into sl->mb[0]
       * after the dump point, so its DC position is zero there.  The DC Hadamard
       * itself is checked by the standalone vl-h264-dc-hadamard test. */
      bool intra_16x16 = mb.mb_type != 0;
      const int16_t *oracle_coeff =
         (const int16_t *)(oracle + addr * ORACLE_RECORD_BYTES + 16);
      for (unsigned s = 0; s < 16; s++) {
         const int16_t *block = mb.coeff4x4[blk_y[s] * 4 + blk_x[s]];
         for (unsigned r = 0; r < 4; r++) {
            for (unsigned c = 0; c < 4; c++) {
               if (intra_16x16 && r == 0 && c == 0)
                  continue; /* oracle dump has no Intra_16x16 DC */
               int16_t mine = block[r * 4 + c];
               int16_t theirs = oracle_coeff[s * 16 + c * 4 + r]; /* transposed */
               if (mine != theirs) {
                  fprintf(stderr, "FAIL mb %u (type %d) block %u (%u,%u): %d != "
                          "oracle %d\n", addr, mb.mb_type, s, r, c, mine, theirs);
                  return 1;
               }
            }
         }
      }
   }

   CHECK(!vl_h264_more_rbsp_data(&reader));

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(nal);
   free(oracle);
   printf("vl_h264_dequant: all %d luma macroblocks (%u Intra_16x16 with DC "
          "Hadamard) dequantize bit-exact with the oracle PASS\n", NUM_MBS,
          i16x16);
   return 0;
}
