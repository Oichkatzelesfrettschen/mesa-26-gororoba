/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Clean-room H.264 macroblock-layer decoder for the Mesa-native CAVLC front end.
 *
 * The slice parser hands per-slice state; this layer walks the macroblock syntax
 * (ITU-T H.264 sec 7.3.5) and fills the non-residual fields of one
 * vl_h264_mb_contract: the macroblock type, the intra prediction modes (with the
 * sec 8.3.1.1 neighbour-predicted mode derivation), the coded block pattern (sec
 * 9.1.2), and the running luma/chroma QP.  The CAVLC residual is a separate
 * stage that fills the coefficients after this header is parsed.
 *
 * A decoder object carries the slice-spanning neighbour state: the running QP
 * and the per-4x4-block intra mode array the mode prediction reads.  Scope is the
 * intra path (I_NxN, I_16x16) needed for the I-frame milestone; the inter
 * macroblock syntax is a later stage.
 */

#ifndef vl_h264_mb_decode_h
#define vl_h264_mb_decode_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_bitstream.h"
#include "vl_h264_mb_contract.h"
#include "vl_h264_slice_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_h264_picture_desc;

struct vl_h264_mb_decoder {
   const struct pipe_h264_picture_desc *picture;
   const struct pipe_h264_sps *sps;
   const struct pipe_h264_pps *pps;
   const struct vl_h264_slice_header *slice;

   unsigned width_in_mbs;
   unsigned height_in_mbs;
   unsigned num_mbs;

   int qp_y;                  /* running luma QP, seeded from the slice header */

   /* Per-4x4-block actual intra mode across the frame, indexed
    * mb_raster * 16 + block_scan; -1 marks a block whose macroblock is not
    * Intra_4x4 (treated as DC-predicting for a neighbour, sec 8.3.1.1). */
   int8_t *intra4x4_modes;
};

/* Initialise the decoder for one frame.  Allocates the neighbour-state arrays;
 * returns false on bad parameters or allocation failure. */
bool vl_h264_mb_decoder_init(struct vl_h264_mb_decoder *dec,
                             const struct pipe_h264_picture_desc *picture,
                             unsigned width_in_mbs, unsigned height_in_mbs);

void vl_h264_mb_decoder_fini(struct vl_h264_mb_decoder *dec);

/* Begin a slice: seed the running QP from the slice header and record it. */
void vl_h264_mb_decoder_begin_slice(struct vl_h264_mb_decoder *dec,
                                    const struct vl_h264_slice_header *slice);

/*
 * Decode one macroblock's header (sec 7.3.5) at raster position (mb_x, mb_y) into
 * mb, up to and including mb_qp_delta; the residual stage runs after.  Fills
 * mb_type, intra4x4_pred_mode / intra_chroma_pred_mode, cbp_luma / cbp_chroma,
 * transform_8x8, and qp_y / qp_cb / qp_cr.  Returns false for an unsupported
 * macroblock type (the inter path is a later stage).
 */
bool vl_h264_decode_mb_header(struct vl_h264_mb_decoder *dec,
                              struct vl_h264_reader *reader,
                              unsigned mb_x, unsigned mb_y,
                              struct vl_h264_mb_contract *mb);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_mb_decode_h */
