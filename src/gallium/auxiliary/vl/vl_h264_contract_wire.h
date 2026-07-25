/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Versioned little-endian wire format for the H.264 per-macroblock contract.
 *
 * One source of truth for serializing struct vl_h264_slice_contract so the
 * steinmarder libavcodec oracle (Python) and the in-driver replay provider (C)
 * exchange the exact same bytes.  The format mirrors struct vl_h264_mb_contract
 * field for field, in declaration order, with explicit widths and no reliance
 * on C struct padding: the integer metadata as int32, the coefficients as int16
 * (matching the contract and the FP24 transform's integer-exact range), the
 * reference indices as int8, and the prediction modes and deblock parameters as
 * the bytes the struct declares.  Everything is little-endian on the wire.
 *
 * Layout:
 *   slice header: magic 'H','2','6','4' | version (u32) | width (i32) |
 *                 height (i32) | slice_type (i32) | provider (i32) |
 *                 coeff_contract (i32) | num_macroblocks (u32)
 *   then num_macroblocks records, each:
 *     mb_x, mb_y, slice_type, mb_type, qp_y, qp_cb, qp_cr, transform_8x8,
 *     cbp_luma, cbp_chroma                              (10 x i32)
 *     coeff4x4[24][16]                                  (384 x i16)
 *     coeff8x8[4][64]                                   (256 x i16)
 *     mv_l0[16][2], mv_l1[16][2]                        (64 x i16)
 *     ref_l0[16], ref_l1[16]                            (32 x i8)
 *     intra4x4_pred_mode[16], intra_chroma_pred_mode    (17 x u8)
 *     disable_deblock_idc, slice_alpha_c0_offset_div2,
 *     slice_beta_offset_div2                            (3 x i8)
 *
 * The contract carries no prediction: the GPU back half reconstructs intra
 * prediction from already-decoded neighbours and inter prediction by motion
 * compensation, so prediction never crosses this boundary.
 */

#ifndef vl_h264_contract_wire_h
#define vl_h264_contract_wire_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FOURCC 'H','2','6','4' read little-endian. */
#define VL_H264_CONTRACT_WIRE_MAGIC 0x34363248u

/* Wire size of a slice header and a single macroblock record. */
#define VL_H264_CONTRACT_WIRE_HEADER_BYTES (8 * 4)
#define VL_H264_CONTRACT_WIRE_MB_BYTES \
   (10 * 4 + (384 + 256 + 64) * 2 + (32 + 17 + 3))

/* Total bytes a slice with num_macroblocks records occupies on the wire. */
size_t
vl_h264_contract_wire_size(uint32_t num_macroblocks);

/*
 * Serialize slice into buf (at least vl_h264_contract_wire_size bytes).  Returns
 * the number of bytes written, or 0 if buf is too small.
 */
size_t
vl_h264_contract_serialize(const struct vl_h264_slice_contract *slice,
                           uint8_t *buf, size_t buf_size);

/*
 * Deserialize a wire buffer into slice, allocating slice->macroblocks (the
 * caller frees it).  Returns true on a well-formed buffer with the expected
 * magic, version, and length; false otherwise (slice is left untouched).
 */
bool
vl_h264_contract_deserialize(const uint8_t *buf, size_t size,
                             struct vl_h264_slice_contract *slice);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_contract_wire_h */
