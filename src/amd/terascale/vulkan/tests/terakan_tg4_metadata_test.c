/* SPDX-License-Identifier: MIT */

#include "terakan_tg4_metadata.h"

#include <stdio.h>
#include <stdlib.h>

static bool
expect_metadata_index(char const * const label, struct terakan_tg4_metadata_map const * const map,
                      unsigned const resource_index, unsigned const expected_metadata_index)
{
   unsigned metadata_index = UINT32_MAX;
   if (!terakan_tg4_metadata_index(map, resource_index, &metadata_index) ||
       metadata_index != expected_metadata_index) {
      fprintf(stderr, "%s: resource=%u metadata=%u expected=%u\n", label, resource_index,
              metadata_index, expected_metadata_index);
      return false;
   }
   return true;
}

int
main(void)
{
   struct terakan_tg4_metadata_map vertex_map;
   struct terakan_tg4_metadata_map fragment_map;
   terakan_tg4_metadata_map_init(&vertex_map);
   terakan_tg4_metadata_map_init(&fragment_map);

   if (!terakan_tg4_metadata_map_add_range(&vertex_map, 42, 0, 24) ||
       !terakan_tg4_metadata_map_add_range(&fragment_map, 7, 0, 1)) {
      fputs("failed to construct legal TG4 metadata maps\n", stderr);
      return EXIT_FAILURE;
   }

   if (!expect_metadata_index("absolute RID above metadata budget", &vertex_map, 42, 0) ||
       !expect_metadata_index("dynamic array reaches compact slot 23", &vertex_map, 65, 23) ||
       !expect_metadata_index("fragment stage retains its own slot zero", &fragment_map, 7, 0))
      return EXIT_FAILURE;

   unsigned rejected_index;
   if (terakan_tg4_metadata_index(&vertex_map, 7, &rejected_index)) {
      fputs("unmapped absolute resource index was accepted\n", stderr);
      return EXIT_FAILURE;
   }

   uint32_t view_swizzles[MESA_SHADER_STAGES][TERAKAN_TG4_METADATA_DWORD_COUNT] = {{0}};
   if (!terakan_tg4_metadata_set_swizzle(view_swizzles, MESA_SHADER_VERTEX, 0, 0x3210u) ||
       !terakan_tg4_metadata_set_swizzle(view_swizzles, MESA_SHADER_FRAGMENT, 0, 0x0123u) ||
       (view_swizzles[MESA_SHADER_VERTEX][0] & 0xffffu) != 0x3210u ||
       (view_swizzles[MESA_SHADER_FRAGMENT][0] & 0xffffu) != 0x0123u) {
      fputs("stage-local swizzle tables alias slot zero\n", stderr);
      return EXIT_FAILURE;
   }

   struct terakan_tg4_metadata_map overflow_map;
   terakan_tg4_metadata_map_init(&overflow_map);
   if (terakan_tg4_metadata_map_add_range(&overflow_map, 10,
                                          TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES - 1u, 2) ||
       terakan_tg4_metadata_map_add_range(&overflow_map,
                                          TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE - 1u, 0, 2)) {
      fputs("metadata or resource overflow was accepted\n", stderr);
      return EXIT_FAILURE;
   }

   puts("PASS: TG4 stage metadata and absolute resource mapping");
   return EXIT_SUCCESS;
}
