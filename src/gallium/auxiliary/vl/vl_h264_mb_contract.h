/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Per-macroblock CPU-VLD contract for the r300-class FP24 H.264 back half.
 *
 * The decoder is split: a CPU provider does the serial entropy decode (the part
 * that does not map to the fragment ALU) and hands the GPU one record per
 * macroblock; the FP24 fragment programs do the inverse transform, motion
 * compensation, and deblock.  This header is the boundary between the two.  It
 * is provider-agnostic on purpose -- the same record is produced by the
 * libavcodec oracle today and by the clean-room Mesa-native CAVLC/CABAC
 * providers later, so the GPU back half never depends on which front end ran.
 *
 * Canonical coefficient orientation.  Coefficients are stored raster, index
 * row*N + col, in the pixel-natural orientation: applying the H.264 inverse
 * transform to a block and adding it to the prediction reconstructs the image
 * upright, with no transpose in the fragment program.  This is a contract, not
 * an accident of any one front end.  libavcodec's internal sl->mb buffer stores
 * each block transposed relative to this, so the oracle adapter transposes on
 * the way in; a Mesa-native provider emits canonical orientation directly.  The
 * transpose lives in the provider adapter and never in the GPU path -- baking a
 * front-end's storage quirk into the shader would force every other provider to
 * match it backwards.
 */

#ifndef vl_h264_mb_contract_h
#define vl_h264_mb_contract_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the record layout changes; providers and the GPU back half must
 * agree on the same value. */
#define VL_H264_MB_CONTRACT_VERSION 1

/* 4:2:0 luma has sixteen 4x4 blocks; chroma adds four Cb and four Cr. */
#define VL_H264_LUMA_4X4_BLOCKS   16
#define VL_H264_CHROMA_4X4_BLOCKS 8
#define VL_H264_TOTAL_4X4_BLOCKS \
   (VL_H264_LUMA_4X4_BLOCKS + VL_H264_CHROMA_4X4_BLOCKS)

/* The 8x8 transform is a High-profile luma-only path in 4:2:0: the 16x16 luma
 * is four 8x8 blocks, and chroma stays 4x4.  A macroblock uses coeff4x4 or
 * coeff8x8 for luma per transform_8x8, never both. */
#define VL_H264_LUMA_8X8_BLOCKS 4

/* Which front end produced the record.  The GPU back half is identical across
 * all of them; only the entropy decode differs. */
enum vl_h264_vld_provider_kind {
   VL_H264_VLD_PROVIDER_FFMPEG_ORACLE = 0, /* libavcodec instrumentation; not for upstream */
   VL_H264_VLD_PROVIDER_MESA_CAVLC = 1,    /* clean-room Mesa-native CAVLC */
   VL_H264_VLD_PROVIDER_MESA_CABAC = 2,    /* clean-room Mesa-native CABAC */
};

/* Where the dequant happens.  DEQUANTIZED hands the back half coefficients the
 * inverse transform consumes directly -- the fastest transform bring-up.
 * RAW_LEVELS hands raw levels plus qp and the GPU does the dequant. */
enum vl_h264_coeff_contract {
   VL_H264_COEFF_DEQUANTIZED = 0,
   VL_H264_COEFF_RAW_LEVELS = 1,
};

enum vl_h264_slice_type {
   VL_H264_SLICE_P = 0,
   VL_H264_SLICE_B = 1,
   VL_H264_SLICE_I = 2,
   VL_H264_SLICE_SP = 3,
   VL_H264_SLICE_SI = 4,
};

/*
 * One macroblock, everything the FP24 back half needs and nothing the entropy
 * decoder kept to itself.  Coefficients are int16: dequantized values exceed
 * the int8 range, and the FP24 transform is integer-exact only while a
 * coefficient stays under the s1e7m16 mantissa limit (the 4x4 envelope reaches
 * ~17250, the 8x8 envelope is tighter at ~6800 because its butterfly adds a
 * right-shift-by-two stage; above either the block needs a CPU fallback).
 */
struct vl_h264_mb_contract {
   int32_t mb_x;             /* raster macroblock position */
   int32_t mb_y;
   int32_t slice_type;       /* enum vl_h264_slice_type */
   int32_t mb_type;          /* provider-native macroblock type bits */
   int32_t qp_y;             /* luma quantization parameter, 0..51 */
   int32_t qp_cb;
   int32_t qp_cr;
   int32_t transform_8x8;    /* nonzero: luma uses coeff8x8, else coeff4x4 */
   int32_t cbp_luma;         /* coded-block pattern, luma 8x8 quadrants */
   int32_t cbp_chroma;

   /* Canonical raster [row*4 + col], pixel-natural.  Sixteen luma blocks then
    * four Cb and four Cr.  Used for luma when transform_8x8 is zero; always
    * used for chroma. */
   int16_t coeff4x4[VL_H264_TOTAL_4X4_BLOCKS][16];

   /* Canonical raster [row*8 + col], pixel-natural.  Four luma 8x8 blocks, used
    * for luma when transform_8x8 is nonzero. */
   int16_t coeff8x8[VL_H264_LUMA_8X8_BLOCKS][64];

   /* Inter prediction, per 4x4 luma block: motion vectors in quarter-pel and
    * reference indices (-1 = not used) for list 0 and list 1. */
   int16_t mv_l0[16][2];
   int16_t mv_l1[16][2];
   int8_t ref_l0[16];
   int8_t ref_l1[16];

   /* Intra prediction modes: per-4x4 luma mode and the chroma mode. */
   uint8_t intra4x4_pred_mode[16];
   uint8_t intra_chroma_pred_mode;

   /* Deblock parameters from the slice header; the filter strength per edge is
    * derived on the GPU from these plus the neighbour coding. */
   int8_t disable_deblock_idc;
   int8_t slice_alpha_c0_offset_div2;
   int8_t slice_beta_offset_div2;

   /* Raster address of the first macroblock of this macroblock's slice
    * (first_mb_in_slice).  Intra prediction may use a neighbor only
    * from the same slice; Constrained Baseline disables FMO, so a
    * slice is a contiguous ascending macroblock range and a neighbor at raster
    * address n shares this slice when n >= slice_first_mb.  The CPU-VLD provider
    * stamps this field per macroblock; the wire and replay path leaves it zero,
    * correct for the single-slice fixtures that path carries. */
   int32_t slice_first_mb;
};

/* A ref_pic_list_modification command (sec 7.3.3.1): modification_of_pic_nums_idc
 * 0 and 1 reorder short-term references by an abs_diff_pic_num, 2 selects a
 * long-term reference by long_term_pic_num.  value carries
 * abs_diff_pic_num_minus1 (idc 0/1) or long_term_pic_num (idc 2). */
#define VL_H264_MAX_REORDER_L0 32
struct vl_h264_ref_reorder {
   uint8_t idc;
   uint32_t value;
};

/* One slice's worth of macroblocks plus the provider/contract identity the back
 * half validates before consuming the records. */
struct vl_h264_slice_contract {
   uint32_t version;         /* VL_H264_MB_CONTRACT_VERSION */
   int32_t width;
   int32_t height;
   int32_t slice_type;       /* enum vl_h264_slice_type */
   int32_t provider;         /* enum vl_h264_vld_provider_kind */
   int32_t coeff_contract;   /* enum vl_h264_coeff_contract */
   uint32_t num_macroblocks;
   struct vl_h264_mb_contract *macroblocks;

   /* RefPicList0 reordering for the frame's reconstructed slice (sec 8.2.4.3.1).
    * Zero when ref_pic_list_modification_flag_l0 is 0.  Single-slice frames carry
    * one slice's commands; the back half and the multiref fixups read the
    * reordered list0 in end_frame. */
   uint32_t num_reorder_l0;
   struct vl_h264_ref_reorder reorder_l0[VL_H264_MAX_REORDER_L0];
};

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_mb_contract_h */
