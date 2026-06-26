/*
 * Copyright (c) 2026 Terascale Functionalists
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
 * residual, and writes Clip1 back so later blocks read it.
 */
void vl_h264_intra_reconstruct_luma(const struct vl_h264_mb_contract *mbs,
                                    unsigned num_mbs, unsigned width_in_mbs,
                                    unsigned height_in_mbs, uint8_t *luma,
                                    unsigned stride);

/*
 * The Intra_4x4 prediction for one block (sec 8.3.1.2), exposed for validation:
 * build the neighbour samples from the reconstructed luma plane for block s of
 * the macroblock at (mb_x, mb_y), run prediction mode, and write the 4x4
 * prediction in raster order (pred[y*4 + x]).
 */
void vl_h264_intra_predict_4x4(const uint8_t *luma, unsigned stride,
                               unsigned width_in_mbs, unsigned height_in_mbs,
                               unsigned mb_x, unsigned mb_y, unsigned s, int mode,
                               int16_t pred[16]);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_intra_reconstruct_h */
