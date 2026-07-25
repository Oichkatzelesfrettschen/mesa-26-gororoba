/*
 * SPDX-License-Identifier: MIT
 */

/*
 * File-driven whole-frame H.264 inter reconstruction on a softpipe screen.
 *
 * Where vl_h264_decode_test drives one synthetic macroblock against an integer
 * spec, this drives a real frame: it reads a serialized slice contract built by
 * the libavcodec inter oracle (R300_H264_INTER_DUMP -> adapter) and an MB-aligned
 * I420 reference frame, runs begin_frame / decode_bitstream / end_frame through
 * the pipe_video_codec vtable with the replay provider, and writes the
 * reconstructed I420 frame.  A separate tool diffs that against the ffmpeg
 * -skip_loop_filter all decode of the same frame; end_frame reconstructs
 * MC + residual + Clip1 with no deblock, matching the un-deblocked reference.
 *
 * Inputs (environment):
 *   R300_H264_FRAME_W, R300_H264_FRAME_H   MB-aligned frame dimensions.
 *   R300_H264_CONTRACT_REPLAY              serialized contract the provider reads.
 *   R300_H264_FRAME_REF                    MB-aligned I420 reference (frame N-1).
 *   R300_H264_FRAME_OUT                    output MB-aligned I420 reconstruction.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "pipe/p_video_codec.h"
#include "pipe/p_video_state.h"
#include "util/format/u_formats.h"
#include "util/u_inlines.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "vl_video_buffer.h"
#include "vl_h264_decoder.h"

static int W, H, CW, CH;   /* luma W,H; chroma CW,CH */

static struct pipe_video_buffer *
make_buffer(struct pipe_context *ctx)
{
   struct pipe_video_buffer tmpl = {0};
   tmpl.buffer_format = PIPE_FORMAT_NV12;
   tmpl.width = W;
   tmpl.height = H;
   tmpl.interlaced = false;
   return vl_video_buffer_create(ctx, &tmpl);
}

static struct pipe_resource *
plane(struct pipe_video_buffer *vb, int i)
{
   struct pipe_resource *res[VL_NUM_COMPONENTS] = {0};
   vb->get_resources(vb, res);
   return res[i];
}

static void
upload_luma(struct pipe_context *ctx, struct pipe_video_buffer *vb, const uint8_t *y)
{
   struct pipe_transfer *xfer;
   uint8_t *map = pipe_texture_map(ctx, plane(vb, 0), 0, 0, PIPE_MAP_WRITE,
                                   0, 0, W, H, &xfer);
   for (int r = 0; r < H; ++r)
      memcpy(map + r * xfer->stride, y + r * W, W);
   pipe_texture_unmap(ctx, xfer);
}

static void
upload_chroma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
              const uint8_t *cb, const uint8_t *cr)
{
   struct pipe_transfer *xfer;
   uint8_t *map = pipe_texture_map(ctx, plane(vb, 1), 0, 0, PIPE_MAP_WRITE,
                                   0, 0, CW, CH, &xfer);
   for (int r = 0; r < CH; ++r) {
      uint8_t *dst = map + r * xfer->stride;
      for (int c = 0; c < CW; ++c) {
         dst[c * 2 + 0] = cb[r * CW + c];
         dst[c * 2 + 1] = cr[r * CW + c];
      }
   }
   pipe_texture_unmap(ctx, xfer);
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

static uint8_t *
read_file(const char *path, size_t want)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   uint8_t *buf = malloc(want);
   size_t got = buf ? fread(buf, 1, want, f) : 0;
   fclose(f);
   if (got != want) { free(buf); return NULL; }
   return buf;
}

int
main(void)
{
   const char *sw = getenv("R300_H264_FRAME_W"), *sh = getenv("R300_H264_FRAME_H");
   const char *ref_path = getenv("R300_H264_FRAME_REF");
   const char *out_path = getenv("R300_H264_FRAME_OUT");
   const char *contract = getenv("R300_H264_CONTRACT_REPLAY");
   if (!sw || !sh || !ref_path || !out_path || !contract) {
      fprintf(stderr, "vl-h264-decode-frame: env not set; skipping\n");
      return 77;
   }
   W = atoi(sw); H = atoi(sh); CW = W / 2; CH = H / 2;

   struct sw_winsys *winsys = null_sw_create();
   struct pipe_screen *screen = winsys ? softpipe_create_screen(winsys) : NULL;
   if (!screen) {
      fprintf(stderr, "vl-h264-decode-frame: no softpipe; skipping\n");
      return 77;
   }
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);

   size_t ysz = (size_t)W * H, csz = (size_t)CW * CH;
   uint8_t *refbuf = read_file(ref_path, ysz + 2 * csz);
   if (!refbuf) {
      fprintf(stderr, "vl-h264-decode-frame: bad reference file\n");
      return 1;
   }

   struct pipe_video_codec templat = {0};
   templat.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE;
   templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
   templat.width = W;
   templat.height = H;
   templat.max_references = 1;

   struct pipe_video_codec *dec = vl_create_h264_decoder(ctx, &templat);
   if (!dec) {
      fprintf(stderr, "vl-h264-decode-frame: decoder create failed\n");
      return 1;
   }

   struct pipe_video_buffer *target = make_buffer(ctx);
   struct pipe_video_buffer *reference = make_buffer(ctx);
   upload_luma(ctx, reference, refbuf);
   upload_chroma(ctx, reference, refbuf + ysz, refbuf + ysz + csz);

   struct pipe_h264_picture_desc pic = {0};
   pic.base.profile = templat.profile;
   pic.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   pic.num_ref_frames = 1;
   pic.ref[0] = reference;

   const uint8_t nal[1] = {0};
   const void *buffers[1] = { nal };
   const unsigned sizes[1] = { 1 };

   dec->begin_frame(dec, target, &pic.base);
   dec->decode_bitstream(dec, target, &pic.base, 1, buffers, sizes);
   dec->end_frame(dec, target, &pic.base);
   ctx->flush(ctx, NULL, 0);

   uint8_t *outy = malloc(ysz), *outcb = malloc(csz), *outcr = malloc(csz);
   readback_luma(ctx, target, outy);
   readback_chroma(ctx, target, outcb, outcr);

   FILE *f = fopen(out_path, "wb");
   bool ok = f != NULL;
   if (ok) {
      ok &= fwrite(outy, 1, ysz, f) == ysz;
      ok &= fwrite(outcb, 1, csz, f) == csz;
      ok &= fwrite(outcr, 1, csz, f) == csz;
      fclose(f);
   }
   printf("vl-h264-decode-frame: %s %dx%d -> %s\n", ok ? "OK" : "WRITE-FAIL",
          W, H, out_path);

   free(outy); free(outcb); free(outcr); free(refbuf);
   target->destroy(target);
   reference->destroy(reference);
   dec->destroy(dec);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);
   return ok ? 0 : 1;
}
