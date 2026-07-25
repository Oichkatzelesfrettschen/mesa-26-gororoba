/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The real H.264 decoder path for an IDR frame: drive begin_frame /
 * decode_bitstream / end_frame through the pipe_video_codec vtable with the Mesa
 * CAVLC provider and no reference frame, and check the reconstructed planes
 * against ffmpeg.  An IDR is all intra, so end_frame reconstructs the whole frame
 * on the CPU and writes it to the target -- the path that an intra-only first
 * frame takes, which previously produced nothing.  Arguments: the IDR NAL, the
 * SPS NAL, the PPS NAL, and ffmpeg's I420 reference frame.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "util/u_inlines.h"

#include "vl_h264_decoder.h"
#include "vl_h264_param_parser.h"
#include "vl_video_buffer.h"

/* Default QCIF; overridable by argv[5]/argv[6] for other-resolution clips. */
static int W = 176, H = 144, CW = 88, CH = 72;

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
plane(struct pipe_video_buffer *vb, int i)
{
   struct pipe_resource *res[VL_NUM_COMPONENTS] = { 0 };
   vb->get_resources(vb, res);
   return res[i];
}

static void
readback_luma(struct pipe_context *ctx, struct pipe_video_buffer *vb, uint8_t *y)
{
   struct pipe_transfer *xfer;
   const uint8_t *map = pipe_texture_map(ctx, plane(vb, 0), 0, 0, PIPE_MAP_READ,
                                         0, 0, W, H, &xfer);
   for (int r = 0; r < H; ++r)
      memcpy(y + r * W, map + r * xfer->stride, W);
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_chroma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
                uint8_t *cb, uint8_t *cr)
{
   struct pipe_transfer *xfer;
   const uint8_t *map = pipe_texture_map(ctx, plane(vb, 1), 0, 0, PIPE_MAP_READ,
                                         0, 0, CW, CH, &xfer);
   for (int r = 0; r < CH; ++r) {
      const uint8_t *src = map + r * xfer->stride;
      for (int c = 0; c < CW; ++c) {
         cb[r * CW + c] = src[c * 2 + 0];
         cr[r * CW + c] = src[c * 2 + 1];
      }
   }
   pipe_texture_unmap(ctx, xfer);
}

int
main(int argc, char **argv)
{
   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct vl_h264_reader reader;

   CHECK(argc > 4);
   if (argc > 6) {
      W = atoi(argv[5]);
      H = atoi(argv[6]);
      CW = W / 2;
      CH = H / 2;
   }
   long idr_n, sps_n, pps_n, ref_n;
   uint8_t *idr = read_file(argv[1], &idr_n);
   uint8_t *sps_nal = read_file(argv[2], &sps_n);
   uint8_t *pps_nal = read_file(argv[3], &pps_n);
   uint8_t *ref = read_file(argv[4], &ref_n);
   CHECK(idr && sps_nal && pps_nal && ref);
   CHECK(ref_n >= W * H + 2 * CW * CH);

   CHECK(vl_h264_reader_init(&reader, sps_nal + 1, (unsigned)(sps_n - 1)));
   CHECK(vl_h264_parse_sps(&reader, &sps));
   vl_h264_reader_fini(&reader);
   CHECK(vl_h264_reader_init(&reader, pps_nal + 1, (unsigned)(pps_n - 1)));
   CHECK(vl_h264_parse_pps(&reader, &sps, &pps));
   vl_h264_reader_fini(&reader);

   struct sw_winsys *winsys = null_sw_create();
   struct pipe_screen *screen = winsys ? softpipe_create_screen(winsys) : NULL;
   if (!screen)
      return 77;
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);

   struct pipe_video_codec templat = { 0 };
   templat.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE;
   templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
   templat.width = W;
   templat.height = H;
   templat.max_references = 1;

   /* With no replay file, the decoder must create with the CAVLC provider. */
   struct pipe_video_codec *dec = vl_create_h264_decoder(ctx, &templat);
   CHECK(dec != NULL);

   struct pipe_video_buffer tmpl = { 0 };
   tmpl.buffer_format = PIPE_FORMAT_NV12;
   tmpl.width = W;
   tmpl.height = H;
   struct pipe_video_buffer *target = vl_video_buffer_create(ctx, &tmpl);
   CHECK(target != NULL);

   /* An IDR: the picture carries the parsed SPS/PPS and no reference. */
   struct pipe_h264_picture_desc pic = { 0 };
   pic.base.profile = templat.profile;
   pic.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   pic.pps = &pps;

   const void *buffers[1] = { idr };
   const unsigned sizes[1] = { (unsigned)idr_n };
   dec->begin_frame(dec, target, &pic.base);
   dec->decode_bitstream(dec, target, &pic.base, 1, buffers, sizes);
   dec->end_frame(dec, target, &pic.base);
   ctx->flush(ctx, NULL, 0);

   uint8_t *y = malloc(W * H), *cb = malloc(CW * CH), *cr = malloc(CW * CH);
   readback_luma(ctx, target, y);
   readback_chroma(ctx, target, cb, cr);

   const uint8_t *ref_y = ref, *ref_cb = ref + W * H, *ref_cr = ref_cb + CW * CH;
   int fail = 0;
   for (int i = 0; i < W * H && !fail; i++)
      if (y[i] != ref_y[i]) {
         fprintf(stderr, "FAIL Y[%d]: %d != ffmpeg %d\n", i, y[i], ref_y[i]);
         fail = 1;
      }
   for (int i = 0; i < CW * CH && !fail; i++)
      if (cb[i] != ref_cb[i] || cr[i] != ref_cr[i]) {
         fprintf(stderr, "FAIL chroma[%d]: cb %d/%d cr %d/%d\n", i, cb[i],
                 ref_cb[i], cr[i], ref_cr[i]);
         fail = 1;
      }

   free(y); free(cb); free(cr);
   target->destroy(target);
   dec->destroy(dec);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);
   free(idr); free(sps_nal); free(pps_nal); free(ref);

   if (fail)
      return 1;
   printf("vl_h264_decode_idr: IDR decoded through the pipe_video_codec vtable "
          "with the CAVLC provider, all planes match ffmpeg PASS\n");
   return 0;
}
