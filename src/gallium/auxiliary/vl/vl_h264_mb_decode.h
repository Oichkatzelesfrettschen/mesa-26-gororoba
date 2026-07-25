/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Clean-room H.264 macroblock-layer decoder for the Mesa-native CAVLC front end.
 *
 * The slice parser hands per-slice state; this layer walks the macroblock syntax
 * (ITU-T H.264 sec 7.3.5) and fills the non-residual fields of one
 * vl_h264_mb_contract: the macroblock type, the intra prediction modes (with the
 * sec 8.3.1.1 neighbor-predicted mode derivation), the coded block pattern (sec
 * 9.1.2), and the running luma/chroma QP.  The CAVLC residual is a separate
 * stage that fills the coefficients after this header is parsed.
 *
 * A decoder object carries the slice-spanning neighbor state: the running QP
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

   /* P-slice skip-run accounting (sec 7.3.4): -1 means the next mb_skip_run is
    * unread; a non-negative value is the count of P_Skip macroblocks still
    * pending before the next coded macroblock. */
   int skip_run;

   /* Per-4x4-block actual intra mode across the frame, indexed
    * mb_raster * 16 + block_scan; -1 marks a block whose macroblock is not
    * Intra_4x4 (treated as DC-predicting for a neighbor, sec 8.3.1.1). */
   int8_t *intra4x4_modes;

   /* Per-4x4-block luma TotalCoeff across the frame, indexed
    * mb_raster * 16 + block_scan; the CAVLC nC neighbor context reads it
    * (sec 9.2.1).  Only meaningful for an already decoded macroblock. */
   uint8_t *nz_luma;

   /* Per-chroma-AC-block TotalCoeff across the frame, indexed
    * (mb_raster * 2 + component) * 4 + chroma_block, for the chroma AC nC
    * neighbor context (sec 9.2.1). */
   uint8_t *nz_chroma_ac;

   /* Per-4x4-block list-0 motion vector (quarter-pel) and reference index
    * across the frame, indexed mb_raster * 16 + raster_block.  The motion
    * vector prediction (sec 8.4.1.3) reads its neighbors from here; ref -1
    * marks an intra or not-yet-decoded block. */
   int16_t *mv_l0_frame;
   int8_t *ref_l0_frame;
};

/* Initialise the decoder for one frame.  Allocates the neighbor-state arrays;
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

/*
 * Decode the intra macroblock body (sec 7.3.5) for the already-positioned mb:
 * the I_NxN / I_16x16 prediction modes, coded block pattern, and mb_qp_delta for
 * the given intra mb_type.  Shared by the I-slice header and the P-slice
 * intra-macroblock path (which passes mb_type after subtracting the five P
 * types).  Returns false for I_PCM or a truncated stream.
 */
bool vl_h264_decode_intra_mb_body(struct vl_h264_mb_decoder *dec,
                                  struct vl_h264_reader *reader, unsigned mb_x,
                                  unsigned mb_y, struct vl_h264_mb_contract *mb,
                                  unsigned mb_type);

/* Chroma QP from luma QP and the picture chroma QP offset (sec 8.5.8, Table
 * 8-15), shared by the intra and inter macroblock paths. */
int vl_h264_chroma_qp_from_luma(int qp_y, int offset);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_mb_decode_h */
