/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Clean-room H.264 CAVLC variable-length-code decode for the Mesa-native front
 * end.  These are the context-selected codeword decoders the residual block
 * parser is built from: coeff_token (ITU-T H.264 Table 9-5), total_zeros (Tables
 * 9-7/9-8 for 4x4, 9-9 for chroma DC), and run_before (Table 9-10).  level_prefix
 * is a unary leading-zero count the bitstream reader decodes directly (Table
 * 9-6), so it is not here.
 *
 * The codeword tables themselves are in vl_h264_cavlc_tables.h, generated
 * mechanically from the spec PDF; a bounded prefix matcher reads them.
 */

#ifndef vl_h264_cavlc_h
#define vl_h264_cavlc_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Coefficient capacity of a residual block.  H.264 sec 7.4.5.3.2 caps
 * TotalCoeff at 16 for every block type, so one bound covers level[], run[],
 * and coeff[] alike, and the decoder rejects a max_num_coeff above it. */
enum { VL_H264_CAVLC_MAX_COEFF = 16 };

/* One residual block's decode result.  level and run are in reverse-significance
 * order (the order the levels are coded); coeff is the quantized coefficient
 * level placed in zig-zag scan order, the form the dequant stage consumes. */
struct vl_h264_cavlc_block {
   unsigned total_coeff;
   unsigned trailing_ones;
   unsigned total_zeros;
   int16_t level[VL_H264_CAVLC_MAX_COEFF];
   uint8_t run[VL_H264_CAVLC_MAX_COEFF];
   int16_t coeff[VL_H264_CAVLC_MAX_COEFF];
};

/*
 * coeff_token (sec 9.2.1): decode TotalCoeff and TrailingOnes for a residual
 * block, selecting the table by the neighbour context nC (nC == -1 is chroma DC;
 * nC >= 8 is the fixed-length code).  Returns false on a codeword no table entry
 * matches or an impossible TrailingOnes > TotalCoeff.
 */
bool vl_h264_cavlc_coeff_token(struct vl_h264_reader *reader, int nc,
                               unsigned *total_coeff, unsigned *trailing_ones);

/*
 * total_zeros (sec 9.2.3): decode the number of zeros before the last non-zero
 * coefficient, the table selected by total_coeff and whether the block is a
 * chroma DC 2x2 (max_num_coeff == 4) or a 4x4 block.  Only called when
 * total_coeff is in [1, max_num_coeff).
 */
bool vl_h264_cavlc_total_zeros(struct vl_h264_reader *reader,
                               unsigned total_coeff, unsigned max_num_coeff,
                               unsigned *total_zeros);

/*
 * run_before (sec 9.2.3): decode the run of zeros before a coefficient, the
 * table selected by zeros_left (a single table for zeros_left > 6).  Only called
 * when zeros_left > 0.
 */
bool vl_h264_cavlc_run_before(struct vl_h264_reader *reader,
                              unsigned zeros_left, unsigned *run_before);

/*
 * Level decode (sec 9.2.2): decode the total_coeff levels in reverse-significance
 * order into level[], the first trailing_ones of them being the +/-1 trailing
 * ones.  Carries the suffixLength state machine.  Exposed so it can be tested in
 * isolation, the most off-by-one-prone part of CAVLC.
 */
bool vl_h264_cavlc_decode_levels(struct vl_h264_reader *reader,
                                 unsigned total_coeff, unsigned trailing_ones,
                                 int16_t level[VL_H264_CAVLC_MAX_COEFF]);

/*
 * Decode one residual block (sec 7.3.5.3.1): coeff_token (by nC), the levels,
 * total_zeros, and run_before, combined (sec 9.2.4) into out->coeff in scan
 * order.  max_num_coeff is 16 for a 4x4 luma/AC block, 15 for an AC block that
 * skips the DC, 4 for a chroma DC 2x2.  Returns false on a malformed codeword or
 * an out-of-range coefficient position, leaving out usable only on true.  A
 * max_num_coeff above VL_H264_CAVLC_MAX_COEFF is rejected before out is touched,
 * so a caller-side capacity error leaves the caller's block intact.
 */
bool vl_h264_cavlc_residual_block(struct vl_h264_reader *reader,
                                  unsigned max_num_coeff, int nc,
                                  struct vl_h264_cavlc_block *out);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_cavlc_h */
