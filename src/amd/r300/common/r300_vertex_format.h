/*
 * SPDX-License-Identifier: MIT
 */

#ifndef AMD_R300_VERTEX_FORMAT_H
#define AMD_R300_VERTEX_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* API-neutral R300 vertex-format identity.  Gallium and Vulkan adapters map
 * their public formats to this enum; route admission is deliberately separate
 * from the format's mechanical fetch and default-component semantics. */
enum r300_vertex_format_id {
   R300_VERTEX_FORMAT_INVALID = 0,
   R300_VERTEX_FORMAT_F32_1,
   R300_VERTEX_FORMAT_F32_2,
   R300_VERTEX_FORMAT_F32_3,
   R300_VERTEX_FORMAT_F32_4,
   R300_VERTEX_FORMAT_COUNT,
};

enum r300_vertex_numeric_class {
   R300_VERTEX_NUMERIC_SFLOAT = 0,
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
         .physical_components = 4,
         .semantic_record_bytes = 16,
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
