/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The Mesa CAVLC provider gate: the clean-room front end is now the active
 * VL_H264_VLD provider, so this checks the provider wiring rather than the decode
 * math the component tests already cover.  It confirms the provider reports
 * available, that decoding an IDR and a P slice through the provider produces the
 * same per-macroblock contracts as the direct front-end path, that the provider
 * sets the slice type and fills the frame, and that a malformed slice fails
 * closed.  Arguments: the IDR NAL, the P-slice NAL, the SPS NAL, and the PPS NAL.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_dequant.h"
#include "vl_h264_inter.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_param_parser.h"
#include "vl_h264_slice_parser.h"
#include "vl_h264_vld_provider.h"

#define WIDTH_IN_MBS 11
#define HEIGHT_IN_MBS 9
#define NUM_MBS (WIDTH_IN_MBS * HEIGHT_IN_MBS)

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

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

/* The direct front-end path the provider should reproduce: parse the slice
 * header, decode every macroblock, and dequantize, against the same picture. */
static bool
direct_decode(const struct pipe_h264_picture_desc *picture, const uint8_t *nal,
              long nal_size, struct vl_h264_mb_contract *mbs)
{
   struct vl_h264_reader reader;
   struct vl_h264_slice_header sh;
   struct vl_h264_mb_decoder dec;
   if (!vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)))
      return false;
   if (!vl_h264_parse_slice_header(&reader, picture, (nal[0] >> 5) & 3,
                                   nal[0] & 0x1f, &sh)) {
      vl_h264_reader_fini(&reader);
      return false;
   }
   if (!vl_h264_mb_decoder_init(&dec, picture, WIDTH_IN_MBS, HEIGHT_IN_MBS)) {
      vl_h264_reader_fini(&reader);
      return false;
   }
   vl_h264_mb_decoder_begin_slice(&dec, &sh);
   bool p = sh.slice_type == VL_H264_SLICE_P;
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;
      struct vl_h264_mb_residual res;
      bool coded = true;
      if (p) {
         enum vl_h264_p_mb_kind kind =
            vl_h264_decode_p_mb(&dec, &reader, mb_x, mb_y, &mbs[addr]);
         coded = kind != VL_H264_P_MB_SKIP;
         if (kind == VL_H264_P_MB_ERROR) break;
      } else if (!vl_h264_decode_mb_header(&dec, &reader, mb_x, mb_y, &mbs[addr])) {
         break;
      }
      if (coded) {
         vl_h264_decode_mb_luma_residual(&dec, &reader, mb_x, mb_y, &mbs[addr], &res);
         vl_h264_decode_mb_chroma_residual(&dec, &reader, mb_x, mb_y, &mbs[addr], &res);
         vl_h264_dequant_fill_contract(&res, &mbs[addr]);
      } else {
         memset(mbs[addr].coeff4x4, 0, sizeof(mbs[addr].coeff4x4));
      }
   }
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);
   return true;
}

static int
check_slice(struct vl_h264_vld_provider *provider,
            const struct pipe_h264_picture_desc *picture, const uint8_t *nal,
            long nal_size, enum vl_h264_slice_type want_type)
{
   struct vl_h264_mb_contract *via_provider = calloc(NUM_MBS, sizeof(*via_provider));
   struct vl_h264_mb_contract *via_direct = calloc(NUM_MBS, sizeof(*via_direct));
   CHECK(via_provider && via_direct);

   struct vl_h264_slice_contract frame = { 0 };
   frame.version = VL_H264_MB_CONTRACT_VERSION;
   frame.width = WIDTH_IN_MBS * 16;
   frame.height = HEIGHT_IN_MBS * 16;
   frame.num_macroblocks = NUM_MBS;
   frame.macroblocks = via_provider;

   CHECK(provider->decode_slice(provider, picture, nal, (unsigned)nal_size,
                                &frame));
   CHECK(frame.slice_type == (int)want_type);
   CHECK(direct_decode(picture, nal, nal_size, via_direct));
   for (unsigned a = 0; a < NUM_MBS; a++) {
      const uint8_t *pp = (const uint8_t *)&via_provider[a];
      const uint8_t *dd = (const uint8_t *)&via_direct[a];
      for (size_t b = 0; b < sizeof(via_provider[a]); b++)
         if (pp[b] != dd[b]) {
            fprintf(stderr, "FAIL mb %u byte %zu of %zu: provider %d != direct %d "
                    "(mb_type p%d d%d)\n", a, b, sizeof(via_provider[a]), pp[b],
                    dd[b], via_provider[a].mb_type, via_direct[a].mb_type);
            return 1;
         }
   }

   free(via_provider);
   free(via_direct);
   return 0;
}

int
main(int argc, char **argv)
{
   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct pipe_h264_picture_desc pic;
   struct vl_h264_reader reader;
   uint8_t *idr, *pnal, *sps_nal, *pps_nal;
   long idr_n, p_n, sps_n, pps_n;

   CHECK(argc > 4);
   idr = read_file(argv[1], &idr_n);
   pnal = read_file(argv[2], &p_n);
   sps_nal = read_file(argv[3], &sps_n);
   pps_nal = read_file(argv[4], &pps_n);
   CHECK(idr && pnal && sps_nal && pps_nal);

   CHECK(vl_h264_reader_init(&reader, sps_nal + 1, (unsigned)(sps_n - 1)));
   CHECK(vl_h264_parse_sps(&reader, &sps));
   vl_h264_reader_fini(&reader);
   CHECK(vl_h264_reader_init(&reader, pps_nal + 1, (unsigned)(pps_n - 1)));
   CHECK(vl_h264_parse_pps(&reader, &sps, &pps));
   vl_h264_reader_fini(&reader);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   /* Factory: the Mesa CAVLC provider is available and is the preferred one. */
   CHECK(vl_h264_vld_provider_available(VL_H264_VLD_PROVIDER_MESA_CAVLC));
   CHECK(vl_h264_vld_provider_any_available());
   struct vl_h264_vld_provider *provider = vl_h264_vld_provider_create_available();
   CHECK(provider && provider->kind == VL_H264_VLD_PROVIDER_MESA_CAVLC);

   /* The provider reproduces the direct front-end contracts for I and P. */
   if (check_slice(provider, &pic, idr, idr_n, VL_H264_SLICE_I))
      return 1;
   if (check_slice(provider, &pic, pnal, p_n, VL_H264_SLICE_P))
      return 1;

   /* A malformed slice fails closed: a one-byte NAL has no parsable header. */
   {
      struct vl_h264_mb_contract mb = { 0 };
      struct vl_h264_slice_contract frame = { 0 };
      frame.version = VL_H264_MB_CONTRACT_VERSION;
      frame.num_macroblocks = 1;
      frame.macroblocks = &mb;
      uint8_t truncated[2] = { idr[0], 0x00 };
      CHECK(!provider->decode_slice(provider, &pic, truncated, 2, &frame));
   }

   provider->destroy(provider);
   free(idr);
   free(pnal);
   free(sps_nal);
   free(pps_nal);
   printf("vl_h264_cavlc_provider: MESA_CAVLC available and preferred; provider "
          "reproduces the direct front-end contracts for I and P; malformed slice "
          "fails closed PASS\n");
   return 0;
}
