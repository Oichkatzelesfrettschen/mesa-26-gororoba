/* SPDX-License-Identifier: MIT */

#ifndef TERAKAN_TG4_METADATA_H
#define TERAKAN_TG4_METADATA_H

#include "terakan_descriptor.h"

#include "compiler/shader_enums.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TERAKAN_TG4_METADATA_INDEX_INVALID UINT8_MAX
#define TERAKAN_TG4_METADATA_DWORD_COUNT   (TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES / 2u)

struct terakan_tg4_metadata_map {
   uint8_t resource_to_metadata[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
};

static inline void
terakan_tg4_metadata_map_init(struct terakan_tg4_metadata_map * const map)
{
   memset(map->resource_to_metadata, TERAKAN_TG4_METADATA_INDEX_INVALID,
          sizeof(map->resource_to_metadata));
}

static inline bool
terakan_tg4_metadata_map_add_range(struct terakan_tg4_metadata_map * const map,
                                   unsigned const first_resource, unsigned const first_metadata,
                                   unsigned const descriptor_count)
{
   if (first_resource > TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE ||
       descriptor_count > TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE - first_resource ||
       first_metadata > TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES ||
       descriptor_count > TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES - first_metadata)
      return false;

   for (unsigned descriptor_index = 0; descriptor_index < descriptor_count; ++descriptor_index) {
      if (map->resource_to_metadata[first_resource + descriptor_index] !=
          TERAKAN_TG4_METADATA_INDEX_INVALID)
         return false;
   }

   for (unsigned descriptor_index = 0; descriptor_index < descriptor_count; ++descriptor_index) {
      map->resource_to_metadata[first_resource + descriptor_index] =
         first_metadata + descriptor_index;
   }
   return true;
}

static inline bool
terakan_tg4_metadata_index(struct terakan_tg4_metadata_map const * const map,
                           unsigned const resource_index, unsigned * const metadata_index_out)
{
   if (resource_index >= TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE)
      return false;

   uint8_t const metadata_index = map->resource_to_metadata[resource_index];
   if (metadata_index == TERAKAN_TG4_METADATA_INDEX_INVALID)
      return false;

   *metadata_index_out = metadata_index;
   return true;
}

static inline bool
terakan_tg4_metadata_set_swizzle(
   uint32_t view_swizzles[MESA_SHADER_STAGES][TERAKAN_TG4_METADATA_DWORD_COUNT],
   mesa_shader_stage const stage, unsigned const metadata_index, uint16_t const packed_swizzle)
{
   if (stage >= MESA_SHADER_STAGES || metadata_index >= TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES)
      return false;

   uint32_t * const dword = &view_swizzles[stage][metadata_index / 2u];
   uint32_t const shift = (metadata_index & 1u) != 0 ? 16u : 0u;
   *dword = (*dword & ~(0xffffu << shift)) | ((uint32_t)packed_swizzle << shift);
   return true;
}

#endif
