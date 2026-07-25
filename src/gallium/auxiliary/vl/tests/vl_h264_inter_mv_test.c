/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The P-slice macroblock-layer gate: decode a real Constrained-Baseline P slice
 * through the CAVLC front end -- skip runs, inter macroblock types and their
 * partitions, and the intra macroblocks a P slice carries -- and check it stays
 * in bitstream synchronization with libavcodec.  The decisive check is the
 * per-macroblock bit position: the front end's bit count, plus the eight bits of
 * NAL header libavcodec's reader counts that ours does not, must equal the
 * oracle's get_bits_count after every macroblock, and the slice must end exactly
 * when the oracle does.  The reference index structure is checked from the
 * decoded macroblock kind (inter blocks reference list 0, intra blocks do not).
 * Motion vector prediction is a later change, so the motion vectors that already
 * match (those with no inter neighbor) are reported, not asserted.  Arguments:
 * the P-slice NAL, the per-macroblock bit-position dump, and the mv/ref dump.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_inter.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_param_parser.h"
#include "vl_h264_slice_parser.h"

#define WIDTH_IN_MBS 11
#define HEIGHT_IN_MBS 9
#define NUM_MBS (WIDTH_IN_MBS * HEIGHT_IN_MBS)
#define NAL_HEADER_BITS 8 /* the oracle's reader counts the NAL header byte */
#define MV_RECORD 88      /* mb_x,mb_y (2 i32) + mv0[32 i16] + ref0[16 i8] */

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

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
   uint8_t *nal, *sps_nal, *pps_nal, *bitpos, *mvref;
   long nal_size, sps_size, pps_size, bitpos_size, mvref_size;

   CHECK(argc > 4);
   nal = read_file(argv[1], &nal_size);
   sps_nal = read_file(argv[2], &sps_size);
   pps_nal = read_file(argv[3], &pps_size);
   bitpos = read_file(argv[4], &bitpos_size);
   mvref = argc > 5 ? read_file(argv[5], &mvref_size) : NULL;
   CHECK(nal && sps_nal && pps_nal && bitpos);
   CHECK(bitpos_size >= NUM_MBS * 16);

   /* Read the decoder parameters from the clip's own SPS and PPS. */
   CHECK(vl_h264_reader_init(&reader, sps_nal + 1, (unsigned)(sps_size - 1)));
   CHECK(vl_h264_parse_sps(&reader, &sps));
   vl_h264_reader_fini(&reader);
   CHECK(vl_h264_reader_init(&reader, pps_nal + 1, (unsigned)(pps_size - 1)));
   CHECK(vl_h264_parse_pps(&reader, &sps, &pps));
   vl_h264_reader_fini(&reader);

   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (nal[0] >> 5) & 3,
                                    nal[0] & 0x1f, &sh));
   CHECK(sh.slice_type == VL_H264_SLICE_P);
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   const int32_t *bp = (const int32_t *)bitpos;
   unsigned mv_match = 0;
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      struct vl_h264_mb_contract mb;
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;

      enum vl_h264_p_mb_kind kind =
         vl_h264_decode_p_mb(&dec, &reader, mb_x, mb_y, &mb);
      CHECK(kind != VL_H264_P_MB_ERROR);

      if (kind == VL_H264_P_MB_INTER || kind == VL_H264_P_MB_INTRA) {
         struct vl_h264_mb_residual res;
         CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mb,
                                               &res));
         CHECK(vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, &mb,
                                                 &res));
      }

      /* The decisive gate: this macroblock ended at the oracle's bit position. */
      int want_bits = bp[addr * 4 + 3] - NAL_HEADER_BITS;
      int got_bits = (int)vl_h264_bits_consumed(&reader);
      if (got_bits != want_bits) {
         fprintf(stderr, "FAIL mb %u (%u,%u) kind %d: bit %d != oracle %d\n",
                 addr, mb_x, mb_y, kind, got_bits, want_bits);
         return 1;
      }

      /* The reference structure follows from the decoded kind. */
      int8_t want_ref = kind == VL_H264_P_MB_INTRA ? -1 : 0;
      for (unsigned blk = 0; blk < 16; blk++)
         CHECK(mb.ref_l0[blk] == want_ref);

      /* Every inter block's predicted motion vector matches the oracle.  The
       * dump's vectors for an intra macroblock are libavcodec's internal cache,
       * not real motion, so the inter blocks are selected by the decoded ref. */
      if (mvref) {
         const int16_t *omv = (const int16_t *)(mvref + addr * MV_RECORD + 8);
         for (unsigned n = 0; n < 16; n++) {
            unsigned raster = blk_y[n] * 4u + blk_x[n];
            if (mb.ref_l0[raster] != 0)
               continue;
            if (mb.mv_l0[raster][0] != omv[n * 2] ||
                mb.mv_l0[raster][1] != omv[n * 2 + 1]) {
               fprintf(stderr, "FAIL mb %u block %u: mv (%d,%d) != oracle "
                       "(%d,%d)\n", addr, n, mb.mv_l0[raster][0],
                       mb.mv_l0[raster][1], omv[n * 2], omv[n * 2 + 1]);
               return 1;
            }
            mv_match++;
         }
      }
   }

   /* The slice ended exactly when the oracle's did. */
   CHECK(!vl_h264_more_rbsp_data(&reader));

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(nal);
   free(sps_nal);
   free(pps_nal);
   free(bitpos);
   free(mvref);
   printf("vl_h264_inter_mv: P slice decodes in full bitstream sync with the "
          "oracle (skip, inter, intra); ref structure and all %u inter motion "
          "vectors match (median, directional, P_Skip) PASS\n", mv_match);
   return 0;
}
