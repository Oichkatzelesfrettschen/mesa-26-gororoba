/*
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

struct vl_h264_mb_contract;
struct vl_h264_mb_residual;

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

/*
 * Inverse-scan and dequantize the fifteen AC coefficients of a block whose DC is
 * coded separately (Intra_16x16 luma, chroma AC).  ac_scan holds the AC levels in
 * zig-zag order; out receives the dequantized AC in raster order with out[0]
 * (the DC) left zero for the caller to fill.
 */
void vl_h264_dequant_4x4_ac(const int16_t ac_scan[15], int qp, int16_t out[16]);

/*
 * Inverse 4x4 Hadamard and scale the sixteen Intra_16x16 luma DC coefficients
 * (sec 8.5.6).  scan holds the DC levels in zig-zag order; dc receives the
 * scaled DC per 4x4 luma block, indexed by the block's raster grid position
 * (row * 4 + col).
 */
void vl_h264_dequant_luma_dc(const int16_t scan[16], int qp, int16_t dc[16]);

/*
 * Inverse 2x2 Hadamard and scale the four 4:2:0 chroma DC coefficients of one
 * component (sec 8.5.7).  level holds the four DC levels in raster order; dc
 * receives the scaled DC per chroma 4x4 block, indexed by raster grid position.
 */
void vl_h264_dequant_chroma_dc(const int16_t level[4], int qp, int16_t dc[4]);

/*
 * Fill a macroblock contract's coefficients from its decoded residual.  Writes
 * the dequantized luma blocks (I_NxN full, or Intra_16x16 AC plus DC Hadamard)
 * and the 4:2:0 chroma DC and AC into mb->coeff4x4 in canonical raster, the form
 * the back half consumes.
 */
void vl_h264_dequant_fill_contract(const struct vl_h264_mb_residual *res,
                                   struct vl_h264_mb_contract *mb);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_dequant_h */
