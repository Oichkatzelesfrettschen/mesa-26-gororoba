/*
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for R3V Gallium map-range representability.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "../r3v_queue_map_range.h"

static unsigned failures;

#define CHECK(condition, name)           \
   do {                                  \
      if (condition) {                   \
         printf("  ok   - %s\n", name); \
      } else {                           \
         printf("  FAIL - %s\n", name); \
         failures++;                     \
      }                                  \
   } while (0)

static void
check_map_range_representability(void)
{
   const uint64_t max_pipe_box = INT32_MAX;
   const uint64_t above_pipe_box = max_pipe_box + 1;

   CHECK(r3v_map_range_representable(0, 64, 4096),
         "ordinary range is representable");
   CHECK(r3v_map_range_representable(max_pipe_box - 16, 16, UINT_MAX),
         "range ending at INT32_MAX is representable");
   CHECK(!r3v_map_range_representable(0, 64, 63),
         "length larger than the Vulkan buffer is rejected");
   CHECK(!r3v_map_range_representable(0, above_pipe_box, above_pipe_box),
         "length above pipe_box range is rejected");
   CHECK(!r3v_map_range_representable(above_pipe_box, 1,
                                      above_pipe_box + 1),
         "offset above pipe_box range is rejected");
   CHECK(!r3v_map_range_representable(UINT64_MAX - 7, 8, UINT64_MAX),
         "wide offset is rejected before range arithmetic");
   CHECK(!r3v_map_range_representable(48, 16, 63),
         "range past the Vulkan buffer is rejected");
}

int
main(void)
{
   check_map_range_representability();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
