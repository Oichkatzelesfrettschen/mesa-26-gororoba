/*
 * SPDX-License-Identifier: MIT
 */

/*
 * P-slice macroblock layer for the Mesa-native H.264 CAVLC front end.
 *
 * This walks the inter macroblock syntax (ITU-T H.264 sec 7.3.4 skip run, 7.3.5
 * macroblock_layer, 7.3.5.1 mb_pred, 7.3.5.2 sub_mb_pred) for Constrained
 * Baseline P slices: the mb_skip_run, the P macroblock types and their
 * partitions, the list-0 motion vector differences, and the coded block pattern.
 * It fills the contract's motion fields -- mv_l0 in quarter-pel and ref_l0 (0 for
 * an inter block, -1 for intra) -- which the GPU back half consumes for motion
 * compensation.  The residual is decoded by the shared CAVLC stage afterwards,
 * exactly as for an intra macroblock.
 */

#ifndef vl_h264_inter_h
#define vl_h264_inter_h

#include "vl_h264_mb_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

enum vl_h264_p_mb_kind {
   VL_H264_P_MB_ERROR = 0,  /* malformed stream */
   VL_H264_P_MB_SKIP,       /* P_Skip: no residual follows */
   VL_H264_P_MB_INTER,      /* coded inter macroblock; residual follows */
   VL_H264_P_MB_INTRA,      /* intra macroblock inside the P slice */
};

/*
 * Decode the macroblock-layer header of one P-slice macroblock at (mb_x, mb_y):
 * resolve the skip run, parse the inter macroblock type and its motion vectors,
 * fill mb->mv_l0 and mb->ref_l0, and set mb->cbp_luma / mb->cbp_chroma and the
 * running QP.  On VL_H264_P_MB_INTER the caller decodes the residual and fills
 * the contract; on VL_H264_P_MB_SKIP there is no residual.
 */
enum vl_h264_p_mb_kind
vl_h264_decode_p_mb(struct vl_h264_mb_decoder *dec, struct vl_h264_reader *reader,
                    unsigned mb_x, unsigned mb_y, struct vl_h264_mb_contract *mb);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_inter_h */
