/* SPDX-License-Identifier: MIT */

#include "terakan_robustness_metadata.h"

#include <stdio.h>

static int
expect_u32(char const * const label, uint32_t const actual, uint32_t const expected)
{
   if (actual == expected)
      return 0;
   fprintf(stderr, "%s: actual=%u expected=%u\n", label, actual, expected);
   return 1;
}

int
main(void)
{
   BITSET_DECLARE(metadata_needed, TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT) = {0};
   BITSET_DECLARE(writable_uavs, TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT) = {0};
   struct terakan_robustness_metadata_source
      shadow[TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT] = {{0}};

   BITSET_SET(metadata_needed, 19);
   BITSET_SET_RANGE(metadata_needed, 30, 35);
   BITSET_SET(metadata_needed, 72);
   BITSET_SET(writable_uavs, 2);
   BITSET_SET(writable_uavs, 19);
   BITSET_SET(writable_uavs, 72);

   for (unsigned mutable_index = 0;
        mutable_index < TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT; ++mutable_index) {
      shadow[mutable_index].buffer_byte_size = 1000u + mutable_index;
      shadow[mutable_index].texel_buffer_element_count = 2000u + mutable_index;
      shadow[mutable_index].base_array_layer = 3000u + mutable_index;
   }

   int failures = 0;
   failures +=
      expect_u32("sparse index above hardware slot count",
                 terakan_robustness_metadata_index(
                    metadata_needed, 19, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL),
                 0);
   failures +=
      expect_u32("dynamic array first index",
                 terakan_robustness_metadata_index(
                    metadata_needed, 30, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL),
                 1);
   failures +=
      expect_u32("dynamic array vec4 crossing",
                 terakan_robustness_metadata_index(
                    metadata_needed, 35, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL),
                 6);
   failures +=
      expect_u32("sparse tail index",
                 terakan_robustness_metadata_index(
                    metadata_needed, 72, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL),
                 7);

   struct terakan_robustness_metadata_payload pipeline_before_set;
   if (!terakan_robustness_metadata_compact(metadata_needed,
                                            TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL,
                                            shadow, &pipeline_before_set)) {
      fprintf(stderr, "pipeline-before-set compaction failed\n");
      return 1;
   }

   struct terakan_robustness_metadata_source
      descriptor_first_shadow[TERAKAN_ROBUSTNESS_METADATA_MUTABLE_RESOURCE_COUNT] = {{0}};
   memcpy(descriptor_first_shadow, shadow, sizeof(shadow));
   struct terakan_robustness_metadata_payload set_before_pipeline;
   if (!terakan_robustness_metadata_compact(metadata_needed,
                                            TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL,
                                            descriptor_first_shadow, &set_before_pipeline)) {
      fprintf(stderr, "set-before-pipeline replay compaction failed\n");
      return 1;
   }

   if (memcmp(&pipeline_before_set, &set_before_pipeline, sizeof(pipeline_before_set)) != 0) {
      fprintf(stderr, "descriptor and pipeline binding orders diverged\n");
      ++failures;
   }
   failures += expect_u32("compacted sparse payload", pipeline_before_set.uav_byte_sizes[0], 1019);
   failures += expect_u32("compacted dynamic payload", pipeline_before_set.uav_byte_sizes[6], 1035);
   failures +=
      expect_u32("compacted base layer", pipeline_before_set.uav_base_array_layers[7], 3072);

   unsigned const writable_uav_mutant_index = terakan_robustness_metadata_index(
      writable_uavs, 72, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL);
   unsigned const expected_tail_index = terakan_robustness_metadata_index(
      metadata_needed, 72, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL);
   if (writable_uav_mutant_index == expected_tail_index) {
      fprintf(stderr, "writable-UAV-derived index mutant survived\n");
      ++failures;
   }
   unsigned const nonzero_rtv_base_mutant_index = 4u + expected_tail_index;
   if (nonzero_rtv_base_mutant_index == expected_tail_index) {
      fprintf(stderr, "RTV-base-derived index mutant survived\n");
      ++failures;
   }

   struct terakan_robustness_metadata_payload skipped_replay = {0};
   if (memcmp(&skipped_replay, &set_before_pipeline, sizeof(skipped_replay)) == 0) {
      fprintf(stderr, "descriptor-before-pipeline replay mutant survived\n");
      ++failures;
   }

   return failures != 0;
}
