/*
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

/* One macroblock's residual, the producer side of the dequant stage.  All
 * coefficients are quantized and in zig-zag scan order.  For an I_NxN
 * macroblock luma4x4 holds the sixteen full 4x4 blocks; for I_16x16 it holds the
 * fifteen AC coefficients per block and luma_dc holds the sixteen DC
 * coefficients (the Hadamard input).  Chroma is the 2x2 DC per component and the
 * four AC blocks per component for 4:2:0. */
struct vl_h264_mb_residual {
   int16_t luma4x4[16][16];
   int16_t luma_dc[16];
   bool has_luma_dc;
   uint8_t nz_luma[16];

   int16_t chroma_dc[2][4];
   int16_t chroma_ac[2][4][16];
   uint8_t nz_chroma_dc[2];
   uint8_t nz_chroma_ac[2][4];
};

/*
 * Decode the luma residual of the macroblock at (mb_x, mb_y).  mb is the header
 * the syntax decoder filled (mb_type selects I_NxN versus I_16x16, cbp_luma
 * selects which 8x8 groups are coded); res receives the coefficients and counts.
 * Updates the decoder's frame nz_luma store so later macroblocks see this one's
 * neighbour context.  Returns false on a malformed residual block.
 */
bool vl_h264_decode_mb_luma_residual(struct vl_h264_mb_decoder *dec,
                                     struct vl_h264_reader *reader,
                                     unsigned mb_x, unsigned mb_y,
                                     const struct vl_h264_mb_contract *mb,
                                     struct vl_h264_mb_residual *res);

/*
 * Decode the 4:2:0 chroma residual of the macroblock at (mb_x, mb_y): the 2x2 DC
 * per component when cbp_chroma is nonzero, then the four AC blocks per component
 * when cbp_chroma is 2.  Updates the frame nz_chroma_ac store for the AC nC
 * neighbour context.  Returns false on a malformed residual block or an
 * unsupported chroma format.
 */
bool vl_h264_decode_mb_chroma_residual(struct vl_h264_mb_decoder *dec,
                                       struct vl_h264_reader *reader,
                                       unsigned mb_x, unsigned mb_y,
                                       const struct vl_h264_mb_contract *mb,
                                       struct vl_h264_mb_residual *res);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_cavlc_residual_h */
