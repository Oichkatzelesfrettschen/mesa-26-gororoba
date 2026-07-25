/*
 * SPDX-License-Identifier: MIT
 */

/*
 * CPU intra reconstruction for the Mesa-native H.264 front end.
 *
 * H.264 Intra_4x4 prediction is a sub-block wavefront -- block N predicts from
 * block N-1's already reconstructed samples -- so it cannot be split into a
 * standalone prediction plane the GPU adds residual to.  This module reconstructs
 * an intra frame entirely on the CPU: for each macroblock it builds the
 * prediction from neighbour samples (ITU-T H.264 sec 8.3), inverse-transforms the
 * dequantized residual (sec 8.5.12.2), and writes Clip1(prediction + residual)
 * back into the frame plane so the next block reads it.  It is a second consumer
 * of the per-macroblock contract, parallel to the GPU emit path.
 */

#ifndef vl_h264_intra_reconstruct_h
#define vl_h264_intra_reconstruct_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The 4x4 inverse integer transform (sec 8.5.12.2): coeff is a dequantized block
 * in raster order, residual receives the reconstructed residual in raster order.
 * The one-dimensional butterfly realizes the two half-weight basis entries as
 * arithmetic right shifts, applied to rows then columns, with the final
 * (h + 32) >> 6 normalization.
 */
void vl_h264_idct4(const int16_t coeff[16], int16_t residual[16]);

/*
 * Reconstruct the luma plane of an intra frame from its per-macroblock contracts
 * (sec 8.3).  mbs holds num_mbs contracts in raster order; luma is the
 * width_in_mbs*16 by height_in_mbs*16 plane, stride bytes per row.  Each
 * macroblock predicts from already reconstructed neighbours -- Intra_4x4 as a
 * sub-block wavefront, Intra_16x16 whole-block -- adds the inverse-transformed
 * residual, and writes Clip1 back so later blocks read it.  When
 * constrained_intra is set, an inter-coded neighbor is excluded from the
 * prediction sample derivation.
 */
void vl_h264_intra_reconstruct_luma(const struct vl_h264_mb_contract *mbs,
                                    unsigned num_mbs, unsigned width_in_mbs,
                                    unsigned height_in_mbs, uint8_t *luma,
                                    unsigned stride, bool constrained_intra);

/*
 * The Intra_4x4 prediction for one block (sec 8.3.1.2), exposed for validation:
 * build the neighbor samples from the reconstructed luma plane for block s of
 * the macroblock at (mb_x, mb_y), run prediction mode, and write the 4x4
 * prediction in raster order (pred[y*4 + x]).  slice_first_mb is the raster
 * address of the first macroblock of this macroblock's slice; a neighbor in an
 * earlier slice is unavailable.  Pass 0 for a single-slice frame.
 */
void vl_h264_intra_predict_4x4(const uint8_t *luma, unsigned stride,
                               unsigned width_in_mbs, unsigned height_in_mbs,
                               unsigned mb_x, unsigned mb_y, unsigned s,
                               unsigned slice_first_mb, int mode,
                               int16_t pred[16]);

/*
 * Reconstruct the two 4:2:0 chroma planes of an intra frame from its
 * per-macroblock contracts (sec 8.3.3).  cb and cr are the width_in_mbs*8 by
 * height_in_mbs*8 planes, stride bytes per row.  Both components of a macroblock
 * share intra_chroma_pred_mode; each predicts from its neighbour row and column,
 * adds the inverse-transformed residual (chroma DC Hadamard plus AC), and writes
 * Clip1.  When constrained_intra is set, an inter-coded neighbor is excluded
 * from the prediction.
 */
void vl_h264_intra_reconstruct_chroma(const struct vl_h264_mb_contract *mbs,
                                      unsigned num_mbs, unsigned width_in_mbs,
                                      unsigned height_in_mbs, uint8_t *cb,
                                      uint8_t *cr, unsigned stride,
                                      bool constrained_intra);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_intra_reconstruct_h */
