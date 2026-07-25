/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The CPU intra luma reconstruction gate: decode the whole IDR slice through the
 * CAVLC front end, reconstruct the luma plane on the CPU (Intra_4x4 and
 * Intra_16x16 prediction plus the inverse transform), and check it bit-exact
 * against ffmpeg's -skip_loop_filter all reconstruction.  Both paths are integer,
 * so the bar is zero mismatches.
 *
 * The per-block prediction is checked first against the libavcodec sub-block
 * prediction dump (luma Intra_4x4 blocks only): a prediction mismatch localizes a
 * mode, neighbour, or wavefront bug, while a prediction match with a plane
 * mismatch points at the transform, dequant, or clip.  Arguments: the IDR slice
 * NAL, the reference YUV, and the sub-prediction dump.
 */

#include <stdint.h>
#include <stdio.h>
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
#define WIDTH (WIDTH_IN_MBS * 16)
#define HEIGHT (HEIGHT_IN_MBS * 16)
#define SUBPRED_RECORD_BYTES (6 * 4 + 16)

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

/* Find the dumped prediction for block s of the macroblock at (mb_x, mb_y). */
static const uint8_t *
find_subpred(const uint8_t *dump, long size, int mb_x, int mb_y, int s)
{
   for (long off = 0; off + SUBPRED_RECORD_BYTES <= size;
        off += SUBPRED_RECORD_BYTES) {
      const int32_t *h = (const int32_t *)(dump + off);
      if (h[0] == mb_x && h[1] == mb_y && h[2] == s)
         return dump + off + 24;
   }
   return NULL;
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
   struct vl_h264_mb_contract *mbs;
   uint8_t *nal, *ref, *subpred, *luma;
   long nal_size, ref_size, subpred_size;

   CHECK(argc > 3);
   nal = read_file(argv[1], &nal_size);
   CHECK(nal && nal_size > 1);
   ref = read_file(argv[2], &ref_size);
   CHECK(ref && ref_size >= WIDTH * HEIGHT);
   subpred = read_file(argv[3], &subpred_size);
   CHECK(subpred != NULL);

   fill_fixture_sps_pps(&sps, &pps);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (nal[0] >> 5) & 3,
                                    nal[0] & 0x1f, &sh));
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);

   mbs = calloc(NUM_MBS, sizeof(*mbs));
   CHECK(mbs != NULL);
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      struct vl_h264_mb_contract *mb = &mbs[addr];
      struct vl_h264_mb_residual res;
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;

      CHECK(vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, mb));
      CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, mb, &res));
      CHECK(vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, mb,
                                              &res));
      vl_h264_dequant_fill_contract(&res, mb);
   }

   luma = calloc(WIDTH, HEIGHT);
   CHECK(luma != NULL);
   vl_h264_intra_reconstruct_luma(mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                  luma, WIDTH, false);

   /* Localizer: every Intra_4x4 block's prediction matches the oracle dump.  The
    * neighbours come from earlier blocks, never overwritten, so re-deriving from
    * the final plane reproduces the prediction at decode time. */
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      if (mbs[addr].mb_type != 0)
         continue;
      for (unsigned s = 0; s < 16; s++) {
         const uint8_t *dumped = find_subpred(subpred, subpred_size,
                                              mbs[addr].mb_x, mbs[addr].mb_y, s);
         CHECK(dumped != NULL);
         int16_t pred[16];
         vl_h264_intra_predict_4x4(luma, WIDTH, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                   mbs[addr].mb_x, mbs[addr].mb_y, s, 0,
                                   mbs[addr].intra4x4_pred_mode[s], pred);
         for (unsigned i = 0; i < 16; i++) {
            if (pred[i] != dumped[i]) {
               fprintf(stderr, "FAIL mb %u block %u mode %u pred[%u]: %d != "
                       "oracle %d\n", addr, s, mbs[addr].intra4x4_pred_mode[s], i,
                       pred[i], dumped[i]);
               return 1;
            }
         }
      }
   }

   /* The reconstructed plane is bit-exact with ffmpeg's. */
   for (int y = 0; y < HEIGHT; y++) {
      for (int x = 0; x < WIDTH; x++) {
         if (luma[y * WIDTH + x] != ref[y * WIDTH + x]) {
            fprintf(stderr, "FAIL luma (%d,%d): %d != ref %d\n", x, y,
                    luma[y * WIDTH + x], ref[y * WIDTH + x]);
            return 1;
         }
      }
   }

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(mbs);
   free(luma);
   free(nal);
   free(ref);
   free(subpred);
   printf("vl_h264_intra_luma: reconstructed luma plane is bit-exact with ffmpeg "
          "and every Intra_4x4 prediction matches the oracle PASS\n");
   return 0;
}
