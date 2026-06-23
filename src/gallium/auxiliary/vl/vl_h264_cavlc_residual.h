/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Macroblock residual traversal for the Mesa-native H.264 CAVLC front end.
 *
 * The block decoder (vl_h264_cavlc) decodes one residual block; this layer walks
 * a macroblock's residual blocks in the order and with the neighbour context the
 * spec requires (ITU-T H.264 sec 7.3.5.3, 9.2.1).  It reads the coded block
 * pattern from the macroblock header the syntax decoder produced, decodes the
 * sixteen luma 4x4 blocks in 8x8-major scan order, derives each block's nC from
 * the already decoded left and top neighbour blocks, and records each block's
 * TotalCoeff in the frame nz_luma store for the next neighbour.  It outputs the
 * quantized coefficients in scan order; dequant and the inverse scan are later.
 */

#ifndef vl_h264_cavlc_residual_h
#define vl_h264_cavlc_residual_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_bitstream.h"
#include "vl_h264_mb_contract.h"
#include "vl_h264_mb_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One macroblock's residual, the producer side of the dequant stage.  luma4x4 is
 * the quantized coefficients in zig-zag scan order, indexed by the 8x8-major
 * block scan index; nz_luma is each block's TotalCoeff. */
struct vl_h264_mb_residual {
   int16_t luma4x4[16][16];
   uint8_t nz_luma[16];
};

/*
 * Decode the luma residual of the macroblock at (mb_x, mb_y).  mb is the header
 * the syntax decoder filled (cbp_luma selects which 8x8 groups are coded); res
 * receives the coefficients and counts.  Updates the decoder's frame nz_luma
 * store so later macroblocks see this one's neighbour context.  Returns false on
 * a malformed residual block, leaving res partially filled.
 */
bool vl_h264_decode_mb_luma_residual(struct vl_h264_mb_decoder *dec,
                                     struct vl_h264_reader *reader,
                                     unsigned mb_x, unsigned mb_y,
                                     const struct vl_h264_mb_contract *mb,
                                     struct vl_h264_mb_residual *res);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_cavlc_residual_h */
