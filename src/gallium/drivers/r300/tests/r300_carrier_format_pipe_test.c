/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r300_carrier_format_pipe.h"

static unsigned failures;

#define CHECK(COND, NAME)                 \
   do {                                   \
      if (COND) {                         \
         printf("  ok   - %s\n", NAME); \
      } else {                            \
         printf("  FAIL - %s\n", NAME); \
         failures++;                      \
      }                                   \
   } while (0)

int
main(void)
{
   static const struct {
      enum r300_carrier_format carrier;
      enum pipe_format pipe;
      const char *name;
   } cases[] = {
      { R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        PIPE_FORMAT_R8G8B8A8_UNORM, "RGBA8 UNORM" },
      { R300_CARRIER_FORMAT_R32_FLOAT,
        PIPE_FORMAT_R32_FLOAT, "R32 float" },
      { R300_CARRIER_FORMAT_R32G32_FLOAT,
        PIPE_FORMAT_R32G32_FLOAT, "RG32 float" },
      { R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
        PIPE_FORMAT_R32G32B32A32_FLOAT, "RGBA32 float" },
   };

   _Static_assert(sizeof(cases) / sizeof(cases[0]) ==
                     R300_CARRIER_FORMAT_COUNT - 1,
                  "every common carrier format needs a Gallium mapping test");

   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      CHECK(r300_carrier_format_to_pipe(cases[i].carrier) == cases[i].pipe,
            cases[i].name);
   }

   CHECK(r300_carrier_format_to_pipe(R300_CARRIER_FORMAT_INVALID) ==
            PIPE_FORMAT_NONE,
         "invalid common format maps to PIPE_FORMAT_NONE");
   CHECK(r300_carrier_format_to_pipe(R300_CARRIER_FORMAT_COUNT) ==
            PIPE_FORMAT_NONE,
         "common format count sentinel maps to PIPE_FORMAT_NONE");
   CHECK(r300_carrier_format_to_pipe(
            (enum r300_carrier_format)(R300_CARRIER_FORMAT_COUNT + 17)) ==
            PIPE_FORMAT_NONE,
         "unknown common format maps to PIPE_FORMAT_NONE");

   printf("r300 carrier pipe-format adapter: %u failure(s)\n", failures);
   return failures != 0;
}
