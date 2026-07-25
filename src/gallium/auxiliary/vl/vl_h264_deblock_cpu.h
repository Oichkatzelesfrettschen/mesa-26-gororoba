/*
 * SPDX-License-Identifier: MIT
 */

#ifndef VL_H264_DEBLOCK_CPU_H
#define VL_H264_DEBLOCK_CPU_H

#include "vl_h264_mb_contract.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-loop deblock (ITU-T H.264 sec 8.7) over a fully reconstructed frame, in
 * place.  y is the luma plane (mbw*16 by mbh*16) with stride y_stride; cb and cr
 * are the R8 chroma planes (mbw*8 by mbh*8) with stride c_stride.  A NULL plane
 * is skipped.  The boundary strength is derived from the slice contract, so the
 * pass filters intra, inter, and mixed edges in one traversal. */
void vl_h264_deblock_cpu(const struct vl_h264_slice_contract *slice,
                         unsigned mbw, unsigned mbh, uint8_t *y, int y_stride,
                         uint8_t *cb, uint8_t *cr, int c_stride);

#ifdef __cplusplus
}
#endif

#endif /* VL_H264_DEBLOCK_CPU_H */
