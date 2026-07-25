/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "util/u_memory.h"

#include "vl_h264_contract_wire.h"

/* Explicit little-endian cursors, so the wire bytes match the Python oracle on
 * any host without depending on struct layout or host endianness. */

static void
put_u8(uint8_t **cursor, uint8_t value)
{
   *(*cursor)++ = value;
}

static void
put_u16(uint8_t **cursor, uint16_t value)
{
   (*cursor)[0] = value & 0xff;
   (*cursor)[1] = (value >> 8) & 0xff;
   *cursor += 2;
}

static void
put_u32(uint8_t **cursor, uint32_t value)
{
   (*cursor)[0] = value & 0xff;
   (*cursor)[1] = (value >> 8) & 0xff;
   (*cursor)[2] = (value >> 16) & 0xff;
   (*cursor)[3] = (value >> 24) & 0xff;
   *cursor += 4;
}

static uint8_t
get_u8(const uint8_t **cursor)
{
   return *(*cursor)++;
}

static uint16_t
get_u16(const uint8_t **cursor)
{
   uint16_t value = (uint16_t)(*cursor)[0] | ((uint16_t)(*cursor)[1] << 8);
   *cursor += 2;
   return value;
}

static uint32_t
get_u32(const uint8_t **cursor)
{
   uint32_t value = (uint32_t)(*cursor)[0] | ((uint32_t)(*cursor)[1] << 8) |
                    ((uint32_t)(*cursor)[2] << 16) | ((uint32_t)(*cursor)[3] << 24);
   *cursor += 4;
   return value;
}

size_t
vl_h264_contract_wire_size(uint32_t num_macroblocks)
{
   return VL_H264_CONTRACT_WIRE_HEADER_BYTES +
          (size_t)num_macroblocks * VL_H264_CONTRACT_WIRE_MB_BYTES;
}

size_t
vl_h264_contract_serialize(const struct vl_h264_slice_contract *slice,
                           uint8_t *buf, size_t buf_size)
{
   uint8_t *cursor = buf;
   uint32_t m;
   int b, k;

   if (buf_size < vl_h264_contract_wire_size(slice->num_macroblocks))
      return 0;

   put_u32(&cursor, VL_H264_CONTRACT_WIRE_MAGIC);
   put_u32(&cursor, slice->version);
   put_u32(&cursor, (uint32_t)slice->width);
   put_u32(&cursor, (uint32_t)slice->height);
   put_u32(&cursor, (uint32_t)slice->slice_type);
   put_u32(&cursor, (uint32_t)slice->provider);
   put_u32(&cursor, (uint32_t)slice->coeff_contract);
   put_u32(&cursor, slice->num_macroblocks);

   for (m = 0; m < slice->num_macroblocks; m++) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];

      put_u32(&cursor, (uint32_t)mb->mb_x);
      put_u32(&cursor, (uint32_t)mb->mb_y);
      put_u32(&cursor, (uint32_t)mb->slice_type);
      put_u32(&cursor, (uint32_t)mb->mb_type);
      put_u32(&cursor, (uint32_t)mb->qp_y);
      put_u32(&cursor, (uint32_t)mb->qp_cb);
      put_u32(&cursor, (uint32_t)mb->qp_cr);
      put_u32(&cursor, (uint32_t)mb->transform_8x8);
      put_u32(&cursor, (uint32_t)mb->cbp_luma);
      put_u32(&cursor, (uint32_t)mb->cbp_chroma);

      for (b = 0; b < VL_H264_TOTAL_4X4_BLOCKS; b++)
         for (k = 0; k < 16; k++)
            put_u16(&cursor, (uint16_t)mb->coeff4x4[b][k]);
      for (b = 0; b < VL_H264_LUMA_8X8_BLOCKS; b++)
         for (k = 0; k < 64; k++)
            put_u16(&cursor, (uint16_t)mb->coeff8x8[b][k]);
      for (b = 0; b < 16; b++) {
         put_u16(&cursor, (uint16_t)mb->mv_l0[b][0]);
         put_u16(&cursor, (uint16_t)mb->mv_l0[b][1]);
      }
      for (b = 0; b < 16; b++) {
         put_u16(&cursor, (uint16_t)mb->mv_l1[b][0]);
         put_u16(&cursor, (uint16_t)mb->mv_l1[b][1]);
      }
      for (b = 0; b < 16; b++)
         put_u8(&cursor, (uint8_t)mb->ref_l0[b]);
      for (b = 0; b < 16; b++)
         put_u8(&cursor, (uint8_t)mb->ref_l1[b]);
      for (b = 0; b < 16; b++)
         put_u8(&cursor, mb->intra4x4_pred_mode[b]);
      put_u8(&cursor, mb->intra_chroma_pred_mode);
      put_u8(&cursor, (uint8_t)mb->disable_deblock_idc);
      put_u8(&cursor, (uint8_t)mb->slice_alpha_c0_offset_div2);
      put_u8(&cursor, (uint8_t)mb->slice_beta_offset_div2);
   }

   return (size_t)(cursor - buf);
}

bool
vl_h264_contract_deserialize(const uint8_t *buf, size_t size,
                             struct vl_h264_slice_contract *slice)
{
   const uint8_t *cursor = buf;
   struct vl_h264_mb_contract *macroblocks;
   uint32_t version, num_mbs, m;
   int b, k;

   /* The header must be present before num_macroblocks can be trusted. */
   if (size < VL_H264_CONTRACT_WIRE_HEADER_BYTES)
      return false;
   if (get_u32(&cursor) != VL_H264_CONTRACT_WIRE_MAGIC)
      return false;
   version = get_u32(&cursor);
   if (version != VL_H264_MB_CONTRACT_VERSION)
      return false;

   slice->version = version;
   slice->width = (int32_t)get_u32(&cursor);
   slice->height = (int32_t)get_u32(&cursor);
   slice->slice_type = (int32_t)get_u32(&cursor);
   slice->provider = (int32_t)get_u32(&cursor);
   slice->coeff_contract = (int32_t)get_u32(&cursor);
   num_mbs = get_u32(&cursor);

   /* Reject a length that does not match the declared macroblock count before
    * allocating or reading any record. */
   if (size != vl_h264_contract_wire_size(num_mbs))
      return false;

   macroblocks = CALLOC(num_mbs, sizeof(*macroblocks));
   if (num_mbs && !macroblocks)
      return false;

   for (m = 0; m < num_mbs; m++) {
      struct vl_h264_mb_contract *mb = &macroblocks[m];

      mb->mb_x = (int32_t)get_u32(&cursor);
      mb->mb_y = (int32_t)get_u32(&cursor);
      mb->slice_type = (int32_t)get_u32(&cursor);
      mb->mb_type = (int32_t)get_u32(&cursor);
      mb->qp_y = (int32_t)get_u32(&cursor);
      mb->qp_cb = (int32_t)get_u32(&cursor);
      mb->qp_cr = (int32_t)get_u32(&cursor);
      mb->transform_8x8 = (int32_t)get_u32(&cursor);
      mb->cbp_luma = (int32_t)get_u32(&cursor);
      mb->cbp_chroma = (int32_t)get_u32(&cursor);

      for (b = 0; b < VL_H264_TOTAL_4X4_BLOCKS; b++)
         for (k = 0; k < 16; k++)
            mb->coeff4x4[b][k] = (int16_t)get_u16(&cursor);
      for (b = 0; b < VL_H264_LUMA_8X8_BLOCKS; b++)
         for (k = 0; k < 64; k++)
            mb->coeff8x8[b][k] = (int16_t)get_u16(&cursor);
      for (b = 0; b < 16; b++) {
         mb->mv_l0[b][0] = (int16_t)get_u16(&cursor);
         mb->mv_l0[b][1] = (int16_t)get_u16(&cursor);
      }
      for (b = 0; b < 16; b++) {
         mb->mv_l1[b][0] = (int16_t)get_u16(&cursor);
         mb->mv_l1[b][1] = (int16_t)get_u16(&cursor);
      }
      for (b = 0; b < 16; b++)
         mb->ref_l0[b] = (int8_t)get_u8(&cursor);
      for (b = 0; b < 16; b++)
         mb->ref_l1[b] = (int8_t)get_u8(&cursor);
      for (b = 0; b < 16; b++)
         mb->intra4x4_pred_mode[b] = get_u8(&cursor);
      mb->intra_chroma_pred_mode = get_u8(&cursor);
      mb->disable_deblock_idc = (int8_t)get_u8(&cursor);
      mb->slice_alpha_c0_offset_div2 = (int8_t)get_u8(&cursor);
      mb->slice_beta_offset_div2 = (int8_t)get_u8(&cursor);
   }

   slice->num_macroblocks = num_mbs;
   slice->macroblocks = macroblocks;
   return true;
}
