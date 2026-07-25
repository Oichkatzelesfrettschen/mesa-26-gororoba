/*
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end H.264 I+P chroma decode on softpipe, against ffmpeg -- the chroma
 * companion to the luma end-to-end test.  The IDR slice is entropy-decoded and
 * its 4:2:0 chroma planes reconstructed on the CPU (the intra path) to build the
 * reference frame; the P slice is entropy-decoded (skip, inter with motion vector
 * prediction, intra) into per-macroblock contracts; then each P-frame chroma
 * component is reconstructed -- the inter macroblocks by the GPU back half
 * (vl_h264_emit_chroma_inter, eighth-pel bilinear motion compensation from the
 * reconstructed reference plus the residual) on a software screen, the intra
 * macroblocks by the CPU intra path -- and compared to ffmpeg's frame 1
 * (-skip_loop_filter all).
 *
 * The chroma kernel is bilinear, not the six-tap luma filter, so there is no
 * 2D-half-pel overflow and no excluded position: every chroma sample is exact.
 *
 * Arguments: the IDR NAL, the P-slice NAL, the SPS NAL, the PPS NAL, and ffmpeg's
 * frame-1 reference YUV (I420).
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
#define CHROMA_W (WIDTH_IN_MBS * 8)
#define CHROMA_H (HEIGHT_IN_MBS * 8)
#define CHROMA_SIZE (CHROMA_W * CHROMA_H)

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

/* Reconstruct one P-frame chroma component: the inter macroblocks by the back
 * half from the reference component, the intra ones by the CPU pass.  block_base
 * selects Cb (16) or Cr (20) in the contract's coefficient blocks. */
static void
reconstruct_component(struct vl_h264_emit *emit, struct pipe_context *ctx,
                      struct pipe_screen *screen,
                      struct vl_h264_mb_contract *back, const uint8_t *ref_plane,
                      unsigned block_base, uint8_t *out)
{
   struct pipe_resource *ref_tex = make_plane(screen, CHROMA_W, CHROMA_H);
   struct pipe_resource *dst = make_plane(screen, CHROMA_W, CHROMA_H);
   upload_plane(ctx, ref_tex, ref_plane, CHROMA_W, CHROMA_H);

   struct pipe_sampler_view *ref_view;
   {
      struct pipe_sampler_view templ;
      u_sampler_view_default_template(&templ, ref_tex, ref_tex->format);
      ref_view = ctx->create_sampler_view(ctx, ref_tex, &templ);
   }
   struct pipe_surface dst_surf = { { 0 } };
   dst_surf.format = dst->format;
   dst_surf.texture = dst;

   struct vl_h264_slice_contract slice = { 0 };
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = CHROMA_W;
   slice.height = CHROMA_H;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = NUM_MBS;
   slice.macroblocks = back;
   vl_h264_emit_chroma_inter(emit, &dst_surf, CHROMA_W, CHROMA_H, ref_view,
                             CHROMA_W, CHROMA_H, &slice, block_base);
   readback_plane(ctx, dst, out, CHROMA_W, CHROMA_H);

   pipe_sampler_view_reference(&ref_view, NULL);
   pipe_resource_reference(&ref_tex, NULL);
   pipe_resource_reference(&dst, NULL);
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
   CHECK(ref_n >= LUMA_W * LUMA_H + 2 * CHROMA_SIZE);

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
   uint8_t *ref_cb = calloc(CHROMA_SIZE, 1), *ref_cr = calloc(CHROMA_SIZE, 1);
   CHECK(idr_mbs && p_mbs && ref_cb && ref_cr);

   /* The reference chroma: decode the IDR and reconstruct its planes. */
   CHECK(vl_h264_reader_init(&reader, idr + 1, (unsigned)(idr_n - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (idr[0] >> 5) & 3,
                                    idr[0] & 0x1f, &sh));
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);
   CHECK(decode_slice(&dec, &reader, false, idr_mbs));
   vl_h264_intra_reconstruct_chroma(idr_mbs, NUM_MBS, WIDTH_IN_MBS,
                                    HEIGHT_IN_MBS, ref_cb, ref_cr, CHROMA_W,
                                    false);
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);

   /* The P slice. */
   CHECK(vl_h264_reader_init(&reader, pnal + 1, (unsigned)(p_n - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, (pnal[0] >> 5) & 3,
                                    pnal[0] & 0x1f, &sh));
   CHECK(sh.slice_type == VL_H264_SLICE_P);
   CHECK(vl_h264_mb_decoder_init(&dec, &pic, WIDTH_IN_MBS, HEIGHT_IN_MBS));
   vl_h264_mb_decoder_begin_slice(&dec, &sh);
   CHECK(decode_slice(&dec, &reader, true, p_mbs));
   vl_h264_mb_decoder_fini(&dec);
   vl_h264_reader_fini(&reader);

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

   /* The back half writes the whole component; the intra macroblocks go through
    * as a zero-vector copy that the intra pass rewrites. */
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

   uint8_t *cb = calloc(CHROMA_SIZE, 1), *cr = calloc(CHROMA_SIZE, 1);
   CHECK(cb && cr);
   /* Cb is the first four chroma blocks (base 16), Cr the next four (base 20). */
   reconstruct_component(emit, ctx, screen, back, ref_cb,
                         VL_H264_LUMA_4X4_BLOCKS, cb);
   reconstruct_component(emit, ctx, screen, back, ref_cr,
                         VL_H264_LUMA_4X4_BLOCKS + 4, cr);

   /* The intra macroblocks, reading their reconstructed inter neighbors. */
   vl_h264_intra_reconstruct_chroma(p_mbs, NUM_MBS, WIDTH_IN_MBS, HEIGHT_IN_MBS,
                                    cb, cr, CHROMA_W, false);

   const uint8_t *ref_cb_ff = ref + LUMA_W * LUMA_H;
   const uint8_t *ref_cr_ff = ref_cb_ff + CHROMA_SIZE;
   int fail = 0;
   for (int i = 0; i < CHROMA_SIZE && !fail; i++) {
      if (cb[i] != ref_cb_ff[i]) {
         fprintf(stderr, "FAIL Cb [%d]: %d != ffmpeg %d\n", i, cb[i],
                 ref_cb_ff[i]);
         fail = 1;
      }
      if (cr[i] != ref_cr_ff[i]) {
         fprintf(stderr, "FAIL Cr [%d]: %d != ffmpeg %d\n", i, cr[i],
                 ref_cr_ff[i]);
         fail = 1;
      }
   }

   vl_h264_emit_destroy(emit);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);
   free(idr_mbs); free(p_mbs); free(back);
   free(ref_cb); free(ref_cr); free(cb); free(cr);
   free(idr); free(pnal); free(sps_nal); free(pps_nal); free(ref);

   if (fail)
      return 1;
   printf("vl_h264_inter_chroma_e2e: I+P chroma decode on softpipe matches "
          "ffmpeg (%d Cb and Cr samples, %u inter macroblocks, bilinear so no "
          "FP24 gap) PASS\n", 2 * CHROMA_SIZE, n_inter);
   return 0;
}
