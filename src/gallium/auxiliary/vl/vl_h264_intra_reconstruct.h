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

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_intra_reconstruct_h */
