/*
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for R3V Gallium map-range representability.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "../r3v_queue.c"

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
   const uint64_t max_unsigned = UINT_MAX;
   const uint64_t above_max = max_unsigned + 1;

   CHECK(r3v_map_range_representable(0, 64, 4096),
         "ordinary range is representable");
   CHECK(r3v_map_range_representable(max_unsigned - 16, 16,
                                     above_max),
         "range ending at UINT_MAX is representable");
   CHECK(!r3v_map_range_representable(max_unsigned - 15, 16,
                                      above_max),
         "offset plus length overflow is rejected");
   CHECK(!r3v_map_range_representable(0, above_max, above_max),
         "length above UINT_MAX is rejected");
   CHECK(!r3v_map_range_representable(above_max, 1, above_max + 1),
         "offset above UINT_MAX is rejected");
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
