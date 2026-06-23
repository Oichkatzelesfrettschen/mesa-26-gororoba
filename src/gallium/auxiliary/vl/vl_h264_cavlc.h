/*
 * Copyright (c) 2026 Terascale Functionalists
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

#include "vl_h264_bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_cavlc_h */
