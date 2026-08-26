/* SPDX-License-Identifier: MIT */

#ifndef TERAKAN_ROBUSTNESS_METADATA_H
#define TERAKAN_ROBUSTNESS_METADATA_H

#include "terakan_descriptor.h"

#include "util/bitscan.h"
#include "util/bitset.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT                                         \
   MAX2(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL,                                            \
        TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL)

struct terakan_robustness_metadata_source {
   uint32_t buffer_byte_size;
   uint32_t texel_buffer_element_count;
   uint32_t base_array_layer;
};

struct terakan_robustness_metadata_payload {
   uint32_t uav_byte_sizes[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
   uint32_t texel_buffer_element_counts[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
   uint32_t uav_base_array_layers[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
};

static inline unsigned
terakan_robustness_metadata_index(BITSET_WORD const * const needed,
                                  unsigned const mutable_resource_index,
                                  unsigned const mutable_resource_count)
{
   if (mutable_resource_index >= mutable_resource_count ||
       !BITSET_TEST(needed, mutable_resource_index))
      return UINT32_MAX;

   unsigned metadata_index = 0;
   unsigned const first_word = BITSET_BITWORD(mutable_resource_index);
   for (unsigned word_index = 0; word_index < first_word; ++word_index)
      metadata_index += util_bitcount(needed[word_index]);
   metadata_index += util_bitcount(needed[first_word] & (BITSET_BIT(mutable_resource_index) - 1u));

   return metadata_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT ? metadata_index : UINT32_MAX;
}

static inline bool
terakan_robustness_metadata_compact(BITSET_WORD const * const needed,
                                    unsigned const mutable_resource_count,
                                    struct terakan_robustness_metadata_source const * const source,
                                    struct terakan_robustness_metadata_payload * const payload)
{
   memset(payload, 0, sizeof(*payload));

   unsigned metadata_index = 0;
   unsigned mutable_resource_index;
   BITSET_FOREACH_SET (mutable_resource_index, needed, mutable_resource_count) {
      if (metadata_index >= TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT) {
         memset(payload, 0, sizeof(*payload));
         return false;
      }
      payload->uav_byte_sizes[metadata_index] = source[mutable_resource_index].buffer_byte_size;
      payload->texel_buffer_element_counts[metadata_index] =
         source[mutable_resource_index].texel_buffer_element_count;
      payload->uav_base_array_layers[metadata_index] =
         source[mutable_resource_index].base_array_layer;
      ++metadata_index;
   }

   return true;
}

#endif
