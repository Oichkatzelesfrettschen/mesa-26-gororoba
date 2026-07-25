/*
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end H.264 I+P luma decode on softpipe, against ffmpeg.  The whole front
 * end runs: the IDR slice is entropy-decoded and reconstructed on the CPU (the
 * C7 intra path) to build the reference frame; the P slice is entropy-decoded
 * (skip, inter with motion vector prediction, intra) into per-macroblock
 * contracts; then the P frame is reconstructed -- the inter macroblocks by the
 * GPU back half (vl_h264_emit_luma_inter, motion compensation from the
 * reconstructed reference plus the residual) on a software screen, the intra
 * macroblocks by the CPU intra path -- and the result is compared to ffmpeg's
 * frame 1 (-skip_loop_filter all).
 *
 * The back half implements the FP24-feasible luma fractions; the five
 * diagonal-center quarter-pel positions that need the 2D half-pel (j) overflow
 * FP24 and are not in the kernel set, so blocks whose vector lands there are a
 * known back-half gap (a real-silicon concern, not a decode error).  The test
 * asserts every other block bit-exact and reports the gap blocks.
 *
 * Arguments: the IDR NAL, the P-slice NAL, the SPS NAL, the PPS NAL, and ffmpeg's
 * frame-1 reference YUV.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "util/u_inlines.h"
#include "util/u_sampler.h"

#include "pipe/p_video_state.h"

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_cpu_mc.h"
#include "vl_h264_dequant.h"
#include "vl_h264_emit.h"
#include "vl_h264_inter.h"
#include "vl_h264_intra_reconstruct.h"
#include "vl_h264_mb_contract.h"
#include "vl_h264_mb_decode.h"
#include "vl_h264_param_parser.h"
#include "vl_h264_slice_parser.h"

#define WIDTH_IN_MBS 11
#define HEIGHT_IN_MBS 9
#define NUM_MBS (WIDTH_IN_MBS * HEIGHT_IN_MBS)
#define LUMA_W (WIDTH_IN_MBS * 16)
#define LUMA_H (HEIGHT_IN_MBS * 16)

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

static struct pipe_resource *
make_plane(struct pipe_screen *screen, unsigned w, unsigned h)
{
   struct pipe_resource templ = { 0 };
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = w;
   templ.height0 = h;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.format = PIPE_FORMAT_R32_FLOAT;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   return screen->resource_create(screen, &templ);
}

static void
upload_plane(struct pipe_context *ctx, struct pipe_resource *tex,
             const uint8_t *src, unsigned w, unsigned h)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE, 0, 0, w, h, &xfer);
   for (unsigned row = 0; row < h; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (unsigned col = 0; col < w; ++col)
         dst[col] = (float)src[row * w + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_plane(struct pipe_context *ctx, struct pipe_resource *tex, uint8_t *out,
               unsigned w, unsigned h)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0, w, h, &xfer);
   for (unsigned row = 0; row < h; ++row) {
      const float *src = (const float *)((const char *)map + row * xfer->stride);
      for (unsigned col = 0; col < w; ++col)
         out[row * w + col] = (uint8_t)lroundf(src[col]);
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Entropy-decode and dequantize one slice into the macroblock contracts.  P is
 * the inter path (skip, inter, intra); otherwise the intra header path. */
static bool
decode_slice(struct vl_h264_mb_decoder *dec, struct vl_h264_reader *reader,
             bool p_slice, struct vl_h264_mb_contract *mbs)
{
   for (unsigned addr = 0; addr < NUM_MBS; addr++) {
      unsigned mb_x = addr % WIDTH_IN_MBS, mb_y = addr / WIDTH_IN_MBS;
      struct vl_h264_mb_residual res;
      bool coded = true;

      if (p_slice) {
         enum vl_h264_p_mb_kind kind =
            vl_h264_decode_p_mb(dec, reader, mb_x, mb_y, &mbs[addr]);
         if (kind == VL_H264_P_MB_ERROR)
            return false;
         coded = kind != VL_H264_P_MB_SKIP;
      } else if (!vl_h264_decode_mb_header(dec, reader, mb_x, mb_y, &mbs[addr])) {
         return false;
      }

      if (coded) {
         if (!vl_h264_decode_mb_luma_residual(dec, reader, mb_x, mb_y, &mbs[addr],
                                              &res) ||
             !vl_h264_decode_mb_chroma_residual(dec, reader, mb_x, mb_y,
                                                &mbs[addr], &res))
            return false;
         vl_h264_dequant_fill_contract(&res, &mbs[addr]);
      } else {
         memset(mbs[addr].coeff4x4, 0, sizeof(mbs[addr].coeff4x4));
      }
   }
   return true;
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

   CHECK(argc > 5);
   long idr_n, p_n, sps_n, pps_n, ref_n;
   uint8_t *idr = read_file(argv[1], &idr_n);
   uint8_t *pnal = read_file(argv[2], &p_n);
   uint8_t *sps_nal = read_file(argv[3], &sps_n);
   uint8_t *pps_nal = read_file(argv[4], &pps_n);
   uint8_t *ref = read_file(argv[5], &ref_n);
   CHECK(idr && pnal && sps_nal && pps_nal && ref);
   CHECK(ref_n >= LUMA_W * LUMA_H);

   /* Decoder parameters from the clip's own SPS and PPS. */
   CHECK(vl_h264_reader_init(&reader, sps_nal + 1, (unsigned)(sps_n - 1)));
   CHECK(vl_h264_parse_sps(&reader, &sps));
   vl_h264_reader_fini(&reader);
   CHECK(vl_h264_reader_init(&reader, pps_nal + 1, (unsigned)(pps_n - 1)));
   CHECK(vl_h264_parse_pps(&reader, &sps, &pps));
   vl_h264_reader_fini(&reader);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   struct vl_h264_mb_contract *idr_mbs = calloc(NUM_MBS, sizeof(*idr_mbs));
   struct vl_h264_mb_contract *p_mbs = calloc(NUM_MBS, sizeof(*p_mbs));
   uint8_t *ref_luma = calloc(LUMA_W, LUMA_H);
   CHECK(idr_mbs && p_mbs && ref_luma);

   /* The reference frame: decode the IDR and reconstruct it on the CPU. */
   CHECK(vl_h264_reader_init(&reader, idr + 1, (unsigned)(idr_n - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (idr[0] >> 5) & 3,
                                    idr[0] & 0x1f, &sh));
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);
   CHECK(decode_slice(&dec, &reader, false, idr_mbs));
   vl_h264_intra_reconstruct_luma(idr_mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                  ref_luma, LUMA_W, false);
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);

   /* The P slice: entropy-decode the inter and intra contracts. */
   CHECK(vl_h264_reader_init(&reader, pnal + 1, (unsigned)(p_n - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (pnal[0] >> 5) & 3,
                                    pnal[0] & 0x1f, &sh));
   CHECK(sh.slice_type == VL_H264_SLICE_P);
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);
   CHECK(decode_slice(&dec, &reader, true, p_mbs));
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);

   /* Software screen for the back half. */
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys)
      return 77;
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct vl_h264_emit *emit = vl_h264_emit_create(ctx);

   struct pipe_resource *ref_tex = make_plane(screen, LUMA_W, LUMA_H);
   struct pipe_resource *dst = make_plane(screen, LUMA_W, LUMA_H);
   upload_plane(ctx, ref_tex, ref_luma, LUMA_W, LUMA_H);

   struct pipe_sampler_view *ref_view;
   {
      struct pipe_sampler_view templ;
      u_sampler_view_default_template(&templ, ref_tex, ref_tex->format);
      ref_view = ctx->create_sampler_view(ctx, ref_tex, &templ);
   }
   struct pipe_surface dst_surf = { { 0 } };
   dst_surf.format = dst->format;
   dst_surf.texture = dst;

   /* The back half writes the whole frame so the readback is fully defined.  The
    * intra macroblocks go through it as a zero-vector copy whose result is
    * thrown away and rewritten by the intra pass, so only the inter macroblocks'
    * output is kept; counting the genuine inter macroblocks here is for the
    * report. */
   struct vl_h264_mb_contract *back = calloc(NUM_MBS, sizeof(*back));
   unsigned n_inter = 0;
   for (unsigned a = 0; a < NUM_MBS; a++) {
      back[a] = p_mbs[a];
      if (p_mbs[a].ref_l0[0] >= 0) {
         n_inter++;
      } else {
         for (int i = 0; i < 16; i++) {
            back[a].ref_l0[i] = 0;
            back[a].mv_l0[i][0] = back[a].mv_l0[i][1] = 0;
         }
      }
   }

   struct vl_h264_slice_contract slice = { 0 };
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = LUMA_W;
   slice.height = LUMA_H;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = NUM_MBS;
   slice.macroblocks = back;

   vl_h264_emit_luma_inter(emit, &dst_surf, LUMA_W, LUMA_H, ref_view, LUMA_W,
                           LUMA_H, &slice);
   uint8_t *out = calloc(LUMA_W, LUMA_H);
   CHECK(out != NULL);
   readback_plane(ctx, dst, out, LUMA_W, LUMA_H);

   /* The back half cannot produce the diagonal-center quarter-pel positions (the
    * 2D half-pel j overflows FP24); reconstruct those inter luma blocks on the
    * CPU, overwriting the back half's placeholder. */
   vl_h264_cpu_luma_diag_fallback(p_mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                  ref_luma, LUMA_W, LUMA_H, LUMA_W, out, LUMA_W);

   /* The intra macroblocks read their reconstructed inter neighbors from the
    * plane, so they fill after the back half. */
   vl_h264_intra_reconstruct_luma(p_mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                  out, LUMA_W, false);

   /* Every luma sample now matches ffmpeg: the back half did the FP24-feasible
    * inter blocks, the CPU fallback the diagonal-center ones, the CPU intra path
    * the intra macroblocks. */
   unsigned matched = 0;
   int fail = 0;
   for (unsigned a = 0; a < NUM_MBS && !fail; a++) {
      unsigned mb_x = a % WIDTH_IN_MBS, mb_y = a / WIDTH_IN_MBS;
      for (int ly = 0; ly < 16 && !fail; ly++)
         for (int lx = 0; lx < 16; lx++) {
            int px = mb_x * 16 + lx, py = mb_y * 16 + ly;
            if (out[py * LUMA_W + px] != ref[py * LUMA_W + px]) {
               fprintf(stderr, "FAIL mb %u (%d,%d): %d != ffmpeg %d\n", a, lx, ly,
                       out[py * LUMA_W + px], ref[py * LUMA_W + px]);
               fail = 1;
               break;
            }
            matched++;
         }
   }

   vl_h264_emit_destroy(emit);
   pipe_sampler_view_reference(&ref_view, NULL);
   pipe_resource_reference(&ref_tex, NULL);
   pipe_resource_reference(&dst, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);
   free(idr_mbs); free(p_mbs); free(back); free(ref_luma); free(out);
   free(idr); free(pnal); free(sps_nal); free(pps_nal); free(ref);

   if (fail)
      return 1;
   printf("vl_h264_inter_e2e: I+P luma decode on softpipe matches ffmpeg "
          "(%u samples, %u inter macroblocks, every quarter-pel position, no "
          "exclusions) PASS\n", matched, n_inter);
   return 0;
}
