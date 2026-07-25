/*
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end verification of the r300-class H.264 decoder through its
 * pipe_video_codec vtable on a software (softpipe) screen.
 *
 * This drives the whole decode path the way a video frontend would: it builds a
 * one-macroblock inter slice contract, serializes it to the wire format the
 * replay provider reads, creates the decoder (which selects the replay provider
 * because R300_H264_CONTRACT_REPLAY is set), and calls begin_frame,
 * decode_bitstream, and end_frame against real video buffers.  The replay
 * provider feeds the serialized contract to the back half in place of an entropy
 * decoder, so end_frame motion-compensates from the reference, inverse-transforms
 * the residual, and reconstructs the target Y plane.
 *
 * The target Y is read back and checked against an independent integer
 * motion-compensation + inverse-transform + Clip1 + in-loop deblock reference,
 * the same oracle the orchestrator's own harness uses.  The contract carries a
 * nonzero QP so the deblock filter fires, which proves end_frame actually runs
 * the deblock the unorm luma path now wires in and that the contract's QP
 * reaches it through the wire format -- a zero-QP frame would leave the filter a
 * no-op and hide both.  This proves the vtable wiring, the QP round-trip, and
 * the R8_UNORM video-surface boundary, not FP24 truncation (softpipe is f32).
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
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
#include "vl_h264_contract_wire.h"
#include "vl_h264_mb_contract.h"

#include "vl_h264_deblock_ref.h"

#define MB 16

static const int luma_6tap[6] = { 1, -5, 20, 20, -5, 1 };

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

static void
idct4_1d_int(const int64_t z[4], int64_t out[4])
{
   int64_t a = z[0] + z[2];
   int64_t b = z[0] - z[2];
   int64_t c = (z[1] >> 1) - z[3];
   int64_t d = z[1] + (z[3] >> 1);
   out[0] = a + d;
   out[1] = b + c;
   out[2] = b - c;
   out[3] = a - d;
}

static void
idct4_int(const int16_t coeff[16], int64_t residual[16])
{
   int64_t rows[16];
   for (int r = 0; r < 4; ++r) {
      int64_t z[4], o[4];
      for (int c = 0; c < 4; ++c)
         z[c] = coeff[r * 4 + c];
      idct4_1d_int(z, o);
      for (int c = 0; c < 4; ++c)
         rows[r * 4 + c] = o[c];
   }
   for (int c = 0; c < 4; ++c) {
      int64_t z[4], o[4];
      for (int r = 0; r < 4; ++r)
         z[r] = rows[r * 4 + c];
      idct4_1d_int(z, o);
      for (int i = 0; i < 4; ++i)
         residual[i * 4 + c] = (o[i] + 32) >> 6;
   }
}

static int
halfpel_h_ref(const uint8_t *ref, int rw, int rh, int x, int y)
{
   int acc = 0;
   for (int tap = 0; tap < 6; ++tap)
      acc += ref[clampi(y, 0, rh - 1) * rw + clampi(x - 2 + tap, 0, rw - 1)]
             * luma_6tap[tap];
   return clampi((acc + 16) >> 5, 0, 255);
}

static int
predict_ref(const uint8_t *ref, int rw, int rh, int mvx, int mvy, int x, int y)
{
   const int frac_x = mvx & 3, frac_y = mvy & 3;
   const int ox = (mvx >> 2) + x, oy = (mvy >> 2) + y;
   if (frac_x == 2 && frac_y == 0)
      return halfpel_h_ref(ref, rw, rh, ox, oy);
   return ref[clampi(oy, 0, rh - 1) * rw + clampi(ox, 0, rw - 1)];
}

/* Write the slice contract to a temporary file in the serialized wire format the
 * replay provider reads.  Returns the path on success (a static buffer) or NULL. */
static const char *
write_replay_file(const struct vl_h264_slice_contract *slice)
{
   static char path[] = "h264_decode_replay_contract.bin";
   size_t size = vl_h264_contract_wire_size(slice->num_macroblocks);
   uint8_t *buf = malloc(size);
   if (!buf)
      return NULL;
   if (vl_h264_contract_serialize(slice, buf, size) != size) {
      free(buf);
      return NULL;
   }
   FILE *f = fopen(path, "wb");
   if (!f) {
      free(buf);
      return NULL;
   }
   size_t wrote = fwrite(buf, 1, size, f);
   fclose(f);
   free(buf);
   return wrote == size ? path : NULL;
}

static struct pipe_video_buffer *
make_video_buffer(struct pipe_context *ctx)
{
   struct pipe_video_buffer tmpl = {0};
   tmpl.buffer_format = PIPE_FORMAT_NV12;
   tmpl.width = MB;
   tmpl.height = MB;
   tmpl.interlaced = false;
   return vl_video_buffer_create(ctx, &tmpl);
}

static struct pipe_resource *
luma_resource(struct pipe_video_buffer *vb)
{
   struct pipe_resource *res[VL_NUM_COMPONENTS] = {0};
   vb->get_resources(vb, res);
   return res[0];
}

static void
upload_luma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
            const uint8_t *luma)
{
   struct pipe_resource *y = luma_resource(vb);
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, y, 0, 0, PIPE_MAP_WRITE, 0, 0, MB, MB,
                               &xfer);
   for (int row = 0; row < MB; ++row)
      memcpy((char *)map + row * xfer->stride, luma + row * MB, MB);
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_luma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
              uint8_t *out)
{
   struct pipe_resource *y = luma_resource(vb);
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, y, 0, 0, PIPE_MAP_READ, 0, 0, MB, MB,
                               &xfer);
   for (int row = 0; row < MB; ++row)
      memcpy(out + row * MB, (const char *)map + row * xfer->stride, MB);
   pipe_texture_unmap(ctx, xfer);
}

/* 4:2:0 chroma is half resolution and interleaved in NV12 plane 1 (R8G8), Cb in
 * the R lane and Cr in the G lane. */
#define CHROMA (MB / 2)

static struct pipe_resource *
chroma_resource(struct pipe_video_buffer *vb)
{
   struct pipe_resource *res[VL_NUM_COMPONENTS] = {0};
   vb->get_resources(vb, res);
   return res[1];
}

static void
upload_chroma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
              const uint8_t *cb, const uint8_t *cr)
{
   struct pipe_resource *uv = chroma_resource(vb);
   struct pipe_transfer *xfer;
   uint8_t *map = pipe_texture_map(ctx, uv, 0, 0, PIPE_MAP_WRITE, 0, 0, CHROMA,
                                   CHROMA, &xfer);
   for (int row = 0; row < CHROMA; ++row) {
      uint8_t *dst = map + row * xfer->stride;
      for (int col = 0; col < CHROMA; ++col) {
         dst[col * 2 + 0] = cb[row * CHROMA + col];
         dst[col * 2 + 1] = cr[row * CHROMA + col];
      }
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_chroma(struct pipe_context *ctx, struct pipe_video_buffer *vb,
                uint8_t *cb_out, uint8_t *cr_out)
{
   struct pipe_resource *uv = chroma_resource(vb);
   struct pipe_transfer *xfer;
   const uint8_t *map = pipe_texture_map(ctx, uv, 0, 0, PIPE_MAP_READ, 0, 0,
                                         CHROMA, CHROMA, &xfer);
   for (int row = 0; row < CHROMA; ++row) {
      const uint8_t *src = map + row * xfer->stride;
      for (int col = 0; col < CHROMA; ++col) {
         cb_out[row * CHROMA + col] = src[col * 2 + 0];
         cr_out[row * CHROMA + col] = src[col * 2 + 1];
      }
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Eighth-pel chroma bilinear prediction for the macroblock at the origin: the
 * chroma vector is the luma vector in eighth-chroma units (integer >> 3,
 * fraction & 7). */
static int
predict_chroma(const uint8_t *ref, int mvx, int mvy, int lx, int ly)
{
   const int xF = mvx & 7, yF = mvy & 7;
   const int x = lx + (mvx >> 3), y = ly + (mvy >> 3);
   const int a = ref[clampi(y, 0, CHROMA - 1) * CHROMA + clampi(x, 0, CHROMA - 1)];
   const int b = ref[clampi(y, 0, CHROMA - 1) * CHROMA + clampi(x + 1, 0, CHROMA - 1)];
   const int c = ref[clampi(y + 1, 0, CHROMA - 1) * CHROMA + clampi(x, 0, CHROMA - 1)];
   const int d = ref[clampi(y + 1, 0, CHROMA - 1) * CHROMA + clampi(x + 1, 0, CHROMA - 1)];
   const int acc = (8 - xF) * (8 - yF) * a + xF * (8 - yF) * b
                 + (8 - xF) * yF * c + xF * yF * d;
   return (acc + 32) >> 6;
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-decode: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-decode: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);

   /* Reference luma with asymmetric both-axis structure so motion and the
    * half-pel filter are observable (a half-pel-vertical transpose would read a
    * different slope), and smooth enough that the per-block residual is what
    * creates the block-edge steps the deblock filter then smooths. */
   uint8_t ref[MB * MB];
   for (int y = 0; y < MB; ++y)
      for (int x = 0; x < MB; ++x)
         ref[y * MB + x] = (uint8_t)clampi(40 + x * 2 + y * 5, 0, 255);

   /* Reference Cb and Cr planes with their own structure. */
   uint8_t cb_ref[CHROMA * CHROMA], cr_ref[CHROMA * CHROMA];
   for (int y = 0; y < CHROMA; ++y)
      for (int x = 0; x < CHROMA; ++x) {
         cb_ref[y * CHROMA + x] = (uint8_t)((x * 13 + y * 7 + 50) % 256);
         cr_ref[y * CHROMA + x] = (uint8_t)((x * 5 + y * 17 + 90) % 256);
      }

   /* One inter macroblock at the origin: a half-pel-horizontal luma vector (an
    * eighth-pel chroma vector) and non-trivial luma and chroma residuals. */
   const int16_t mvx = 2, mvy = 0;
   struct vl_h264_mb_contract mb;
   memset(&mb, 0, sizeof(mb));
   mb.mb_x = 0;
   mb.mb_y = 0;
   mb.slice_type = VL_H264_SLICE_P;
   /* A nonzero QP so the deblock thresholds (alpha 63, beta 12 at QP 37) are
    * nonzero and the in-loop filter the unorm luma path wires in actually runs;
    * QP reaches the back half only through the serialized contract. */
   mb.qp_y = 37;
   for (int i = 0; i < 16; ++i) {
      mb.mv_l0[i][0] = mvx;
      mb.mv_l0[i][1] = mvy;
      mb.ref_l0[i] = 0;
      mb.ref_l1[i] = -1;
   }
   for (int blk = 0; blk < VL_H264_LUMA_4X4_BLOCKS; ++blk)
      for (int k = 0; k < 16; ++k)
         mb.coeff4x4[blk][k] = (int16_t)(((blk * 7 + k * 13 + 5) % 61) - 30);
   /* Chroma blocks: four Cb then four Cr after the sixteen luma blocks. */
   for (int blk = 0; blk < VL_H264_CHROMA_4X4_BLOCKS; ++blk)
      for (int k = 0; k < 16; ++k)
         mb.coeff4x4[VL_H264_LUMA_4X4_BLOCKS + blk][k] =
            (int16_t)(((blk * 5 + k * 11 + 3) % 41) - 20);

   struct vl_h264_slice_contract slice = {0};
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = MB;
   slice.height = MB;
   slice.slice_type = VL_H264_SLICE_P;
   slice.provider = VL_H264_VLD_PROVIDER_FFMPEG_ORACLE;
   slice.coeff_contract = VL_H264_COEFF_DEQUANTIZED;
   slice.num_macroblocks = 1;
   slice.macroblocks = &mb;

   const char *path = write_replay_file(&slice);
   if (!path) {
      fprintf(stderr, "vl-h264-decode: could not write replay file; skipping\n");
      ctx->destroy(ctx);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }
   setenv("R300_H264_CONTRACT_REPLAY", path, 1);

   struct pipe_video_codec templat = {0};
   templat.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE;
   templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
   templat.width = MB;
   templat.height = MB;
   templat.max_references = 1;

   struct pipe_video_codec *dec = vl_create_h264_decoder(ctx, &templat);
   if (!dec) {
      fprintf(stderr, "vl-h264-decode: decoder create failed; skipping\n");
      unlink(path);
      ctx->destroy(ctx);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   struct pipe_video_buffer *target = make_video_buffer(ctx);
   struct pipe_video_buffer *reference = make_video_buffer(ctx);
   upload_luma(ctx, reference, ref);
   upload_chroma(ctx, reference, cb_ref, cr_ref);

   struct pipe_h264_picture_desc pic = {0};
   pic.base.profile = templat.profile;
   pic.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
   pic.num_ref_frames = 1;
   pic.ref[0] = reference;

   const uint8_t dummy_nal[1] = {0};
   const void *buffers[1] = { dummy_nal };
   const unsigned sizes[1] = { 1 };

   dec->begin_frame(dec, target, &pic.base);
   dec->decode_bitstream(dec, target, &pic.base, 1, buffers, sizes);
   dec->end_frame(dec, target, &pic.base);
   ctx->flush(ctx, NULL, 0);

   uint8_t got[MB * MB];
   readback_luma(ctx, target, got);

   bool pass = true;
   /* Reconstruct the pre-deblock luma (motion compensation plus the inverse-
    * transformed residual, Clip1), then apply the same internal-edge deblock the
    * back half runs; the target Y must match the deblocked reconstruction. */
   int recon[MB * MB];
   for (int y = 0; y < MB; ++y)
      for (int x = 0; x < MB; ++x) {
         int pred = predict_ref(ref, MB, MB, mvx, mvy, x, y);
         int blk = (y / 4) * 4 + (x / 4);
         int64_t res[16];
         idct4_int(mb.coeff4x4[blk], res);
         recon[y * MB + x] = clampi(pred + (int)res[(y % 4) * 4 + (x % 4)], 0, 255);
      }
   unsigned deblock_changed = deblock_reference(recon, MB, MB, &slice);
   for (int y = 0; y < MB && pass; ++y) {
      for (int x = 0; x < MB; ++x) {
         if (got[y * MB + x] != recon[y * MB + x]) {
            printf("FAIL luma (%d,%d) got %d want %d\n", x, y, got[y * MB + x],
                   recon[y * MB + x]);
            pass = false;
            break;
         }
      }
   }
   /* A zero-QP frame (or unwired deblock) would leave the filter a no-op and the
    * end-to-end deblock effect untested; require it to have changed a sample. */
   if (pass && deblock_changed == 0) {
      printf("FAIL luma: deblock changed no samples (vacuous)\n");
      pass = false;
   }

   /* Chroma: the reconstructed Cb and Cr in the interleaved target plane against
    * the eighth-pel prediction plus the inverse-transformed chroma residual. */
   uint8_t got_cb[CHROMA * CHROMA], got_cr[CHROMA * CHROMA];
   readback_chroma(ctx, target, got_cb, got_cr);
   for (int y = 0; y < CHROMA && pass; ++y) {
      for (int x = 0; x < CHROMA; ++x) {
         int blk = (y / 4) * 2 + (x / 4);
         int64_t res_cb[16], res_cr[16];
         idct4_int(mb.coeff4x4[VL_H264_LUMA_4X4_BLOCKS + blk], res_cb);
         idct4_int(mb.coeff4x4[VL_H264_LUMA_4X4_BLOCKS + 4 + blk], res_cr);
         int want_cb = clampi(predict_chroma(cb_ref, mvx, mvy, x, y)
                              + (int)res_cb[(y % 4) * 4 + (x % 4)], 0, 255);
         int want_cr = clampi(predict_chroma(cr_ref, mvx, mvy, x, y)
                              + (int)res_cr[(y % 4) * 4 + (x % 4)], 0, 255);
         if (got_cb[y * CHROMA + x] != want_cb
             || got_cr[y * CHROMA + x] != want_cr) {
            printf("FAIL chroma (%d,%d) Cb got %d want %d, Cr got %d want %d\n",
                   x, y, got_cb[y * CHROMA + x], want_cb,
                   got_cr[y * CHROMA + x], want_cr);
            pass = false;
            break;
         }
      }
   }

   target->destroy(target);
   reference->destroy(reference);
   dec->destroy(dec);
   unlink(path);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-decode: %s (deblock filtered %u luma samples)\n",
          pass ? "PASS" : "FAIL", deblock_changed);
   return pass ? 0 : 1;
}
