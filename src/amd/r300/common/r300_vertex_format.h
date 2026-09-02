/*
 * SPDX-License-Identifier: MIT
 */

#ifndef AMD_R300_VERTEX_FORMAT_H
#define AMD_R300_VERTEX_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* API-neutral R300 vertex-format identity.  Gallium and Vulkan adapters map
 * their public formats to this enum; route admission is deliberately separate
 * from the format's mechanical fetch and default-component semantics. */
enum r300_vertex_format_id {
   R300_VERTEX_FORMAT_INVALID = 0,
   R300_VERTEX_FORMAT_F32_1,
   R300_VERTEX_FORMAT_F32_2,
   R300_VERTEX_FORMAT_F32_3,
   R300_VERTEX_FORMAT_F32_4,
   /* Normalized and half-precision source records.  A vertex record in
    * one of these classes is decoded to binary32 lanes on the host, so
    * the carrier the VAP consumes stays the four-float record the F32
    * family already produces and the route needs no new fetch
    * encoding.  The F32 ids keep values 1 through 4, which
    * r300_vertex_format_from_f32_components returns by component
    * count. */
   R300_VERTEX_FORMAT_UNORM8_1,
   R300_VERTEX_FORMAT_UNORM8_2,
   R300_VERTEX_FORMAT_UNORM8_4,
   /* BGRA component order through the selectors, the one mandatory
    * vertex format whose components are not in memory order. */
   R300_VERTEX_FORMAT_UNORM8_4_BGRA,
   R300_VERTEX_FORMAT_SNORM8_1,
   R300_VERTEX_FORMAT_SNORM8_2,
   R300_VERTEX_FORMAT_SNORM8_4,
   R300_VERTEX_FORMAT_UNORM16_1,
   R300_VERTEX_FORMAT_UNORM16_2,
   R300_VERTEX_FORMAT_UNORM16_4,
   R300_VERTEX_FORMAT_SNORM16_1,
   R300_VERTEX_FORMAT_SNORM16_2,
   R300_VERTEX_FORMAT_SNORM16_4,
   R300_VERTEX_FORMAT_SFLOAT16_1,
   R300_VERTEX_FORMAT_SFLOAT16_2,
   R300_VERTEX_FORMAT_SFLOAT16_4,
   R300_VERTEX_FORMAT_COUNT,
};

/* The source component encoding a gather decodes to a binary32 lane.
 * SFLOAT is the identity: its component is already binary32 and the
 * gather copies its bits. */
enum r300_vertex_numeric_class {
   R300_VERTEX_NUMERIC_SFLOAT = 0,
   R300_VERTEX_NUMERIC_SFLOAT16,
   R300_VERTEX_NUMERIC_UNORM,
   R300_VERTEX_NUMERIC_SNORM,
};

/* R300 VAP_PROG_STREAM_CNTL DATA_TYPE encodings. */
enum r300_vertex_data_type {
   R300_VERTEX_DATA_FLOAT_1 = 0,
   R300_VERTEX_DATA_FLOAT_2 = 1,
   R300_VERTEX_DATA_FLOAT_3 = 2,
   R300_VERTEX_DATA_FLOAT_4 = 3,
};

/* R300 VAP_PROG_STREAM_CNTL_EXT selectors. */
enum r300_vertex_component_select {
   R300_VERTEX_SELECT_X = 0,
   R300_VERTEX_SELECT_Y = 1,
   R300_VERTEX_SELECT_Z = 2,
   R300_VERTEX_SELECT_W = 3,
   R300_VERTEX_SELECT_ZERO = 4,
   R300_VERTEX_SELECT_ONE = 5,
};

struct r300_vertex_format_semantics {
   enum r300_vertex_format_id id;
   enum r300_vertex_numeric_class numeric_class;
   enum r300_vertex_data_type data_type;

   /* Bytes required by the API-visible attribute record. */
   uint8_t physical_components;
   uint8_t semantic_record_bytes;

   /* Bytes one source component occupies, which the selectors index
    * and the numeric class decodes. */
   uint8_t component_bytes;

   /* Dwords the VAP fetches for one record.  This remains independent from
    * semantic_record_bytes even where the current F32 family makes them equal. */
   uint8_t hardware_fetch_dwords;

   /* The producer-facing interface is a logical vec4.  Missing lanes are
    * synthesized by the PSC selectors below, not read from memory. */
   uint8_t logical_components;
   enum r300_vertex_component_select select[4];
};

static inline const struct r300_vertex_format_semantics *
r300_vertex_format_semantics(enum r300_vertex_format_id id)
{
   static const struct r300_vertex_format_semantics formats[] = {
      [R300_VERTEX_FORMAT_F32_1] = {
         .id = R300_VERTEX_FORMAT_F32_1,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 4,
         .physical_components = 1,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_F32_2] = {
         .id = R300_VERTEX_FORMAT_F32_2,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 4,
         .physical_components = 2,
         .semantic_record_bytes = 8,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_F32_3] = {
         .id = R300_VERTEX_FORMAT_F32_3,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT,
         .data_type = R300_VERTEX_DATA_FLOAT_3,
         .component_bytes = 4,
         .physical_components = 3,
         .semantic_record_bytes = 12,
         .hardware_fetch_dwords = 3,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_F32_4] = {
         .id = R300_VERTEX_FORMAT_F32_4,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 4,
         .physical_components = 4,
         .semantic_record_bytes = 16,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_UNORM8_1] = {
         .id = R300_VERTEX_FORMAT_UNORM8_1,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 1,
         .physical_components = 1,
         .semantic_record_bytes = 1,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_UNORM8_2] = {
         .id = R300_VERTEX_FORMAT_UNORM8_2,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 1,
         .physical_components = 2,
         .semantic_record_bytes = 2,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_UNORM8_4] = {
         .id = R300_VERTEX_FORMAT_UNORM8_4,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 1,
         .physical_components = 4,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_UNORM8_4_BGRA] = {
         .id = R300_VERTEX_FORMAT_UNORM8_4_BGRA,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 1,
         .physical_components = 4,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_SNORM8_1] = {
         .id = R300_VERTEX_FORMAT_SNORM8_1,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 1,
         .physical_components = 1,
         .semantic_record_bytes = 1,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SNORM8_2] = {
         .id = R300_VERTEX_FORMAT_SNORM8_2,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 1,
         .physical_components = 2,
         .semantic_record_bytes = 2,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SNORM8_4] = {
         .id = R300_VERTEX_FORMAT_SNORM8_4,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 1,
         .physical_components = 4,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_UNORM16_1] = {
         .id = R300_VERTEX_FORMAT_UNORM16_1,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 2,
         .physical_components = 1,
         .semantic_record_bytes = 2,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_UNORM16_2] = {
         .id = R300_VERTEX_FORMAT_UNORM16_2,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 2,
         .physical_components = 2,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_UNORM16_4] = {
         .id = R300_VERTEX_FORMAT_UNORM16_4,
         .numeric_class = R300_VERTEX_NUMERIC_UNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 2,
         .physical_components = 4,
         .semantic_record_bytes = 8,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_SNORM16_1] = {
         .id = R300_VERTEX_FORMAT_SNORM16_1,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 2,
         .physical_components = 1,
         .semantic_record_bytes = 2,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SNORM16_2] = {
         .id = R300_VERTEX_FORMAT_SNORM16_2,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 2,
         .physical_components = 2,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SNORM16_4] = {
         .id = R300_VERTEX_FORMAT_SNORM16_4,
         .numeric_class = R300_VERTEX_NUMERIC_SNORM,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 2,
         .physical_components = 4,
         .semantic_record_bytes = 8,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
      [R300_VERTEX_FORMAT_SFLOAT16_1] = {
         .id = R300_VERTEX_FORMAT_SFLOAT16_1,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT16,
         .data_type = R300_VERTEX_DATA_FLOAT_1,
         .component_bytes = 2,
         .physical_components = 1,
         .semantic_record_bytes = 2,
         .hardware_fetch_dwords = 1,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_ZERO,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SFLOAT16_2] = {
         .id = R300_VERTEX_FORMAT_SFLOAT16_2,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT16,
         .data_type = R300_VERTEX_DATA_FLOAT_2,
         .component_bytes = 2,
         .physical_components = 2,
         .semantic_record_bytes = 4,
         .hardware_fetch_dwords = 2,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_ZERO, R300_VERTEX_SELECT_ONE },
      },
      [R300_VERTEX_FORMAT_SFLOAT16_4] = {
         .id = R300_VERTEX_FORMAT_SFLOAT16_4,
         .numeric_class = R300_VERTEX_NUMERIC_SFLOAT16,
         .data_type = R300_VERTEX_DATA_FLOAT_4,
         .component_bytes = 2,
         .physical_components = 4,
         .semantic_record_bytes = 8,
         .hardware_fetch_dwords = 4,
         .logical_components = 4,
         .select = { R300_VERTEX_SELECT_X, R300_VERTEX_SELECT_Y,
                     R300_VERTEX_SELECT_Z, R300_VERTEX_SELECT_W },
      },
   };

   if (id <= R300_VERTEX_FORMAT_INVALID || id >= R300_VERTEX_FORMAT_COUNT)
      return NULL;
   return &formats[id];
}

/* One source component decoded to the binary32 bit pattern its lane
 * carries.  SFLOAT copies the component's bits, so the F32 family
 * gathers exactly the bytes it always did.  SFLOAT16 expands IEEE
 * binary16, including its subnormals through the normalizing loop and
 * its infinities and NaNs through the saturated-exponent branch.
 * UNORM divides by the unsigned maximum and SNORM by the positive
 * signed maximum, clamping the one extra negative code to -1, which is
 * the Vulkan fixed-point conversion (Vulkan 1.3, "Conversion from
 * Normalized Fixed-Point to Floating-Point").
 */
static inline uint32_t
r300_vertex_format_decode_component(enum r300_vertex_numeric_class numeric,
                                    unsigned component_bytes,
                                    const uint8_t *src)
{
   uint32_t raw = 0;
   for (unsigned b = 0; b < component_bytes; b++)
      raw |= (uint32_t)src[b] << (b * 8);

   float value;
   switch (numeric) {
   case R300_VERTEX_NUMERIC_SFLOAT:
      return raw;
   case R300_VERTEX_NUMERIC_SFLOAT16: {
      const uint32_t sign = (raw & 0x8000u) << 16;
      uint32_t exponent = (raw >> 10) & 0x1fu;
      uint32_t mantissa = raw & 0x3ffu;
      if (exponent == 0) {
         if (mantissa == 0)
            return sign;
         /* Subnormal: normalize into the binary32 exponent range. */
         exponent = 1;
         while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1;
            exponent--;
         }
         mantissa &= 0x3ffu;
         return sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
      }
      if (exponent == 0x1fu)
         return sign | 0x7f800000u | (mantissa << 13);
      return sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
   }
   case R300_VERTEX_NUMERIC_UNORM: {
      const uint32_t max = component_bytes == 1 ? 0xffu : 0xffffu;
      value = (float)raw / (float)max;
      break;
   }
   case R300_VERTEX_NUMERIC_SNORM: {
      const int32_t bits = component_bytes == 1
                              ? (int32_t)(int8_t)(uint8_t)raw
                              : (int32_t)(int16_t)(uint16_t)raw;
      const float max = component_bytes == 1 ? 127.0f : 32767.0f;
      value = (float)bits / max;
      if (value < -1.0f)
         value = -1.0f;
      break;
   }
   default:
      return 0;
   }

   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static inline enum r300_vertex_format_id
r300_vertex_format_from_f32_components(unsigned components)
{
   return components >= 1 && components <= 4
             ? (enum r300_vertex_format_id)components
             : R300_VERTEX_FORMAT_INVALID;
}

static inline uint16_t
r300_vertex_format_psc_swizzle(
   const struct r300_vertex_format_semantics *format)
{
   if (!format)
      return 0;

   uint16_t swizzle = 0;
   for (unsigned lane = 0; lane < 4; lane++)
      swizzle |= (uint16_t)format->select[lane] << (lane * 3);

   /* R300_WRITE_ENA is four bits at bit 12. */
   return swizzle | (0xfu << 12);
}

#endif /* AMD_R300_VERTEX_FORMAT_H */
