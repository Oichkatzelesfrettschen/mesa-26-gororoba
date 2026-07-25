/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Full I-slice entropy sync test: decode every macroblock of the IDR slice --
 * header, luma residual, chroma residual -- and check that the decode stays in
 * step with the libavcodec oracle to the last bit.
 *
 * Two things are asserted.  First, for every I_NxN macroblock, the sorted
 * multiset of the sixteen luma blocks' TotalCoeff matches the oracle's per-block
 * nonzero counts; I_NxN blocks carry their DC, so the oracle count equals the
 * decoded count.  Second, after the last macroblock the reader sits exactly on
 * the rbsp_stop_one_bit, so the whole slice -- I_NxN and I_16x16 luma, chroma DC
 * and AC -- was consumed bit-exactly.  Any chroma or I_16x16 residual error
 * desyncs the stream and shows up as a later I_NxN count mismatch or a wrong end
 * position, so this validates those paths even where the oracle dump is
 * luma-only.  Arguments: the IDR slice NAL and the oracle macroblock dump.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
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
   sps->pic_width_in_mbs_minus1 = WIDTH_IN_MBS - 1;
   sps->pic_height_in_mbs_minus1 = HEIGHT_IN_MBS - 1;

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

   unsigned nal_ref_idc = (nal[0] >> 5) & 3;
   unsigned nal_unit_type = nal[0] & 0x1f;

   fill_fixture_sps_pps(&sps, &pps);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, nal_ref_idc, nal_unit_type,
                                    &sh));
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   unsigned checked_i_nxn = 0;
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;
      struct vl_h264_mb_contract mb;
      struct vl_h264_mb_residual res;

      memset(&mb, 0, sizeof(mb));
      CHECK(vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, &mb));
      CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mb, &res));
      CHECK(vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, &mb,
                                              &res));

      const uint8_t *record = oracle + addr * ORACLE_RECORD_BYTES;
      int32_t oracle_mb_type;
      memcpy(&oracle_mb_type, record + 8, 4);
      if (!(oracle_mb_type & MB_TYPE_INTRA4X4))
         continue; /* I_16x16 oracle counts include the DC; check via sync */

      const int16_t *coeff = (const int16_t *)(record + 16);
      uint8_t oracle_counts[16], decoded_counts[16];
      for (unsigned b = 0; b < 16; b++) {
         unsigned n = 0;
         for (unsigned k = 0; k < 16; k++)
            n += coeff[b * 16 + k] != 0;
         oracle_counts[b] = (uint8_t)n;
         decoded_counts[b] = res.nz_luma[b];
      }
      qsort(oracle_counts, 16, 1, compare_uint8);
      qsort(decoded_counts, 16, 1, compare_uint8);
      CHECK(memcmp(oracle_counts, decoded_counts, 16) == 0);
      checked_i_nxn++;
   }

   /* The whole slice is consumed: nothing but the trailing bits remains. */
   CHECK(!vl_h264_more_rbsp_data(&reader));
   CHECK(checked_i_nxn > 0);

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(nal);
   free(oracle);
   printf("vl_h264_cavlc_slice: full IDR slice decodes in sync with the oracle "
          "(%u I_NxN macroblocks checked, exact end) PASS\n", checked_i_nxn);
   return 0;
}
