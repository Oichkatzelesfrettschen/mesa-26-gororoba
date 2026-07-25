/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The CPU intra chroma reconstruction gate: decode the whole IDR slice, rebuild
 * the two 4:2:0 chroma planes (DC, Horizontal, Vertical, and Plane prediction
 * plus the inverse transform), and check them bit-exact against ffmpeg's
 * -skip_loop_filter all reconstruction.  This is the first value check of the
 * chroma path end to end -- the DC Hadamard, the chroma residual traversal, and
 * the contract fill -- since the per-macroblock oracle dump is luma-only.  Both
 * paths are integer, so the bar is zero mismatches.  Arguments: the IDR slice NAL
 * and the reference YUV (I420).
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
#define LUMA_W (WIDTH_IN_MBS * 16)
#define LUMA_H (HEIGHT_IN_MBS * 16)
#define CHROMA_W (WIDTH_IN_MBS * 8)
#define CHROMA_H (HEIGHT_IN_MBS * 8)

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
   pps->chroma_qp_index_offset = -2;
   pps->second_chroma_qp_index_offset = -2;
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

static int
compare_plane(const char *name, const uint8_t *got, const uint8_t *ref,
              int width, int height)
{
   for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
         if (got[y * width + x] != ref[y * width + x]) {
            fprintf(stderr, "FAIL %s (%d,%d): %d != ref %d\n", name, x, y,
                    got[y * width + x], ref[y * width + x]);
            return 1;
         }
   return 0;
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
   uint8_t *nal, *ref, *cb, *cr;
   long nal_size, ref_size;

   CHECK(argc > 2);
   nal = read_file(argv[1], &nal_size);
   CHECK(nal && nal_size > 1);
   ref = read_file(argv[2], &ref_size);
   CHECK(ref && ref_size >= LUMA_W * LUMA_H + 2 * CHROMA_W * CHROMA_H);

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
      struct vl_h264_mb_residual res;
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;

      CHECK(vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, &mbs[addr]));
      CHECK(vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mbs[addr],
                                            &res));
      CHECK(vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y,
                                              &mbs[addr], &res));
      vl_h264_dequant_fill_contract(&res, &mbs[addr]);
   }

   cb = calloc(CHROMA_W, CHROMA_H);
   cr = calloc(CHROMA_W, CHROMA_H);
   CHECK(cb && cr);
   vl_h264_intra_reconstruct_chroma(mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS, cb,
                                    cr, CHROMA_W, false);

   /* The reference is I420: Cb (U) then Cr (V) after the luma plane. */
   const uint8_t *ref_cb = ref + LUMA_W * LUMA_H;
   const uint8_t *ref_cr = ref_cb + CHROMA_W * CHROMA_H;
   if (compare_plane("Cb", cb, ref_cb, CHROMA_W, CHROMA_H))
      return 1;
   if (compare_plane("Cr", cr, ref_cr, CHROMA_W, CHROMA_H))
      return 1;

   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   free(mbs);
   free(cb);
   free(cr);
   free(nal);
   free(ref);
   printf("vl_h264_intra_chroma: reconstructed Cb and Cr planes are bit-exact "
          "with ffmpeg PASS\n");
   return 0;
}
