/*
 * SPDX-License-Identifier: MIT
 */

/*
 * CPU luma motion compensation for the quarter-pel positions the FP24 back half
 * cannot produce.  The five diagonal-center fractions (f, i, j, k, q) need the
 * 2D half-pel j, whose intermediate six-tap sum exceeds the FP24 integer-exact
 * range, so the GPU kernel set omits them.  This reconstructs the inter blocks
 * whose vector lands there on the CPU -- full six-tap interpolation (ITU-T H.264
 * sec 8.4.2.2.1) plus the block's residual and Clip1 -- overwriting the back
 * half's placeholder, so a reconstructed frame has no excluded blocks.
 */

#ifndef vl_h264_cpu_mc_h
#define vl_h264_cpu_mc_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether a quarter-pel luma vector lands on a diagonal-center fraction that
 * needs the 2D half-pel j (positions 6, 9, 10, 11, 14 of yFrac*4 + xFrac). */
bool vl_h264_luma_mv_needs_j(int mvx, int mvy);

/*
 * Reconstruct on the CPU the luma blocks whose vector needs the 2D half-pel j,
 * writing Clip1(prediction + residual) into the luma plane.  mbs is the frame's
 * per-macroblock contract; reference is the previous frame's luma plane
 * (ref_w by ref_h, ref_stride bytes per row); luma is the target plane being
 * reconstructed (stride bytes per row).  Only the diagonal-center inter blocks
 * are touched; every other block is the back half's.
 */
void vl_h264_cpu_luma_diag_fallback(const struct vl_h264_mb_contract *mbs,
                                    unsigned num_mbs, unsigned width_in_mbs,
                                    unsigned height_in_mbs,
                                    const uint8_t *reference, int ref_w,
                                    int ref_h, unsigned ref_stride,
                                    uint8_t *luma, unsigned stride);

/* One reference picture's luma plane in RefPicList0 order. */
struct vl_h264_ref_plane {
   const uint8_t *pixels;
   int w, h;
   unsigned stride;
};

/*
 * Reconstruct on the CPU every inter luma block whose back-half result is wrong:
 * a block referencing a RefPicList0 entry past index 0 (the GPU back half samples
 * only refs[0]) at any quarter-pel position, and a block referencing refs[0] at a
 * diagonal-center position that needs the 2D half-pel j.  refs is RefPicList0
 * (refs[0] is the back half's reference); each block's ref_l0 selects the entry.
 * A block referencing refs[0] at a position the back half already produced is left
 * untouched.
 */
void vl_h264_cpu_luma_mc_multiref(const struct vl_h264_mb_contract *mbs,
                                  unsigned num_mbs, unsigned width_in_mbs,
                                  unsigned height_in_mbs,
                                  const struct vl_h264_ref_plane *refs,
                                  unsigned num_refs, uint8_t *luma,
                                  unsigned stride);

/*
 * Reconstruct on the CPU every inter chroma block the back half built from the
 * wrong reference: a 4x4 chroma block one of whose co-located luma blocks selects
 * a RefPicList0 entry past index 0 (the back half samples only refs[0]).  refs_cb
 * and refs_cr are the Cb and Cr planes of RefPicList0 in list order; cb and cr are
 * the target component planes (stride bytes per row).  Prediction is the eighth-pel
 * bilinear (sec 8.4.2.2.2) plus the block residual and Clip1.  A block whose luma
 * blocks all reference refs[0] is left as the back half produced it.
 */
void vl_h264_cpu_chroma_mc_multiref(const struct vl_h264_mb_contract *mbs,
                                    unsigned num_mbs, unsigned width_in_mbs,
                                    unsigned height_in_mbs,
                                    const struct vl_h264_ref_plane *refs_cb,
                                    const struct vl_h264_ref_plane *refs_cr,
                                    unsigned num_refs, uint8_t *cb, uint8_t *cr,
                                    unsigned stride);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_cpu_mc_h */
