/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_mc_h
#define vl_h264_mc_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;

/* H.264 luma half-pel motion compensation (ITU-T H.264 sec 8.4.2.2.1) as FP24
 * fragment programs for the R300-class back half.  A half-pel sample is the
 * six-tap FIR (1, -5, 20, 20, -5, 1) over six integer luma samples along one
 * axis, normalized (sum + 16) >> 5 and clipped to [0, 255].  This is the
 * single-axis case (positions b and h); the 2D diagonal (position j) runs the
 * filter un-normalized on the first axis and overflows the FP24 integer-exact
 * ceiling 2^17 at its achievable peak, so it stays on the CPU.
 *
 * Contract these fragment programs assume from the caller's draw state:
 *   - the reference luma plane is sampler binding 0, filtered NEAREST with
 *     CLAMP_TO_EDGE (the H.264 reference picture is edge-extended; R300 has no
 *     texelFetch, nir_to_rc rejects nir_texop_txf);
 *   - a varying at VARYING_SLOT_VAR0 carries the sample position in .xy
 *     (normalized [0,1]) and the reference texel step in .zw (1/width, 1/height),
 *     so the program reads the six taps at the fixed integer-sample centers
 *     around the position without knowing the plane size.
 *
 * The horizontal program (position b) walks .x by the .z step; the vertical
 * program (position h) walks .y by the .w step. */
void *vl_h264_mc_create_halfpel_h_fs(struct pipe_context *pipe);
void *vl_h264_mc_create_halfpel_v_fs(struct pipe_context *pipe);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_mc_h */
