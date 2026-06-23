/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * H.264 inverse scan and dequantization for the Mesa-native CAVLC front end.
 *
 * The CAVLC stage produces quantized transform-coefficient levels in zig-zag
 * scan order; the back half consumes dequantized coefficients in pixel-natural
 * raster order.  This module bridges them: the inverse zig-zag scan (ITU-T H.264
 * sec 8.5.6) places each level at its raster position, and the dequant
 * (sec 8.5.9, flat scaling lists) multiplies by LevelScale = weightScale *
 * normAdjust with the qP-dependent shift.  weightScale is the flat default 16;
 * custom scaling lists are a later addition.
 */

#ifndef vl_h264_dequant_h
#define vl_h264_dequant_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Inverse-scan and dequantize one full 4x4 block (an I_NxN luma block or any
 * block whose DC is coded in place): scan holds the sixteen levels in zig-zag
 * order, out receives the dequantized coefficients in raster order (row * 4 +
 * col, pixel-natural).  qp is the block's quantization parameter.
 */
void vl_h264_dequant_4x4(const int16_t scan[16], int qp, int16_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_dequant_h */
