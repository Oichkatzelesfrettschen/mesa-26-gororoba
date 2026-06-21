/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Frame reconstruction orchestrator for the r300-class FP24 H.264 back half.
 *
 * The per-stage fragment programs -- inverse transform (vl_h264_idct), motion
 * compensation (vl_h264_mc), and sample reconstruction (vl_h264_reconstruct) --
 * each verify their own arithmetic.  This is the sequencer that drives them over
 * a whole frame from a decoded slice contract, the same role vl_mpeg12_end_frame
 * plays for MPEG-2: scatter the per-macroblock coefficients into a frame
 * coefficient plane, motion-compensate each macroblock's prediction from the
 * reference, inverse-transform the residual for the whole frame in one pass pair,
 * and write Clip1(prediction + residual) into the target.
 *
 * The orchestrator works on plain pipe textures, surfaces, and sampler views, so
 * it is exercised on a software screen without any video-buffer or entropy
 * provider: vl_h264_end_frame extracts the planes from the pipe_video_buffer and
 * calls in here.  All intermediate planes are R32_FLOAT carrying integer sample
 * and coefficient values (the 0..255 luma domain and the integer-exact transform
 * range), matching the per-stage verification rungs; the conversion to and from a
 * UNORM video surface is the caller's boundary.
 */

#ifndef vl_h264_emit_h
#define vl_h264_emit_h

#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct pipe_surface;
struct pipe_sampler_view;
struct vl_h264_emit;

/* Build the shader and state bundle the orchestrator reuses across frames.  The
 * fragment programs are created on pipe, so pass the context the back half draws
 * with (the decoder's multimedia context, or a software context in a test). */
struct vl_h264_emit *vl_h264_emit_create(struct pipe_context *pipe);

void vl_h264_emit_destroy(struct vl_h264_emit *emit);

/*
 * Reconstruct the luma plane of an inter frame from slice into dst_luma.  For
 * each macroblock the orchestrator motion-compensates a prediction from ref_luma
 * at the block's list-0 motion vector, inverse-transforms its sixteen 4x4
 * residual blocks, and writes Clip1(prediction + residual).  dst_luma is the
 * target Y surface (width x height in luma samples); ref_luma is the reference
 * frame's Y sampler view (ref_width x ref_height).
 *
 * Motion is read from the contract's list-0 vectors in quarter-pel.  This first
 * bring-up rung covers integer-pel and the axis-aligned half-pel positions (the
 * existing six-tap kernels); the quarter-pel and diagonal positions, the
 * sub-macroblock partitions, and chroma are separate rungs.  Macroblocks whose
 * list-0 reference is unused are left at the prediction the reference already
 * holds at that position (a zero motion vector), so an all-inter P frame with one
 * reference reconstructs end to end.
 */
void vl_h264_emit_luma_inter(struct vl_h264_emit *emit,
                             struct pipe_surface *dst_luma,
                             unsigned width, unsigned height,
                             struct pipe_sampler_view *ref_luma,
                             unsigned ref_width, unsigned ref_height,
                             const struct vl_h264_slice_contract *slice);

/*
 * The same reconstruction with the video-surface format boundary handled: the
 * reference sampler view and the destination surface are R8_UNORM video planes
 * in [0,1], and this scales into and out of the orchestrator's integer 0..255
 * luma domain.  vl_h264_end_frame calls this with the planes the video buffers
 * expose; the bare vl_h264_emit_luma_inter is the R32_FLOAT core the per-stage
 * harness drives directly.
 */
void vl_h264_emit_luma_inter_unorm(struct vl_h264_emit *emit,
                                   struct pipe_surface *dst_luma,
                                   unsigned width, unsigned height,
                                   struct pipe_sampler_view *ref_luma,
                                   unsigned ref_width, unsigned ref_height,
                                   const struct vl_h264_slice_contract *slice);

/*
 * Reconstruct one chroma component plane (Cb or Cr) of an inter frame.  block_base
 * selects the component's four 4x4 blocks in the contract (16 for Cb, 20 for Cr).
 * Each macroblock's 8x8 prediction is motion-compensated from ref_chroma with the
 * eighth-pel bilinear kernel, the chroma vector being the luma list-0 vector
 * interpreted in eighth-chroma-sample units; the residual is inverse-transformed
 * and Clip1(prediction + residual) is written to dst_chroma.  Planes are
 * half-resolution (the frame chroma dimensions); like vl_h264_emit_luma_inter
 * this is the R32_FLOAT core, and the UNORM video-surface boundary (including the
 * NV12 interleave) is the caller's.
 */
void vl_h264_emit_chroma_inter(struct vl_h264_emit *emit,
                               struct pipe_surface *dst_chroma,
                               unsigned width, unsigned height,
                               struct pipe_sampler_view *ref_chroma,
                               unsigned ref_width, unsigned ref_height,
                               const struct vl_h264_slice_contract *slice,
                               unsigned block_base);

/*
 * Reconstruct both chroma components of an inter frame into one NV12 interleaved
 * R8G8 chroma surface (Cb in the R lane, Cr in the G lane), handling the video-
 * surface format boundary.  ref_chroma is the reference's interleaved chroma
 * view; this de-interleaves and scales it into the integer 0..255 domain,
 * reconstructs Cb and Cr, and recombines the results into dst_chroma.
 * vl_h264_end_frame calls this with the chroma planes the video buffers expose.
 */
void vl_h264_emit_chroma_inter_unorm(struct vl_h264_emit *emit,
                                     struct pipe_surface *dst_chroma,
                                     unsigned width, unsigned height,
                                     struct pipe_sampler_view *ref_chroma,
                                     unsigned ref_width, unsigned ref_height,
                                     const struct vl_h264_slice_contract *slice);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_emit_h */
