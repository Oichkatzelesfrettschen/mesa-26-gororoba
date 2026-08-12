/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r300_defines.h"
#include "r300_state_inlines.h"

static unsigned failures;

#define CHECK(condition, name)                                             \
   do {                                                                     \
      if (!(condition)) {                                                  \
         fprintf(stderr, "FAIL %s: %s\n", name, #condition);              \
         failures++;                                                       \
      }                                                                     \
   } while (0)

static void
check_no_alpha_target(void)
{
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_DST_ALPHA, false,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_ONE,
         "no-alpha RGB DST_ALPHA is ONE");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_INV_DST_ALPHA, false,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_ZERO,
         "no-alpha RGB INV_DST_ALPHA is ZERO");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, false,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_ZERO,
         "no-alpha RGB SRC_ALPHA_SATURATE is ZERO");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, true,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_ONE,
         "no-alpha alpha SRC_ALPHA_SATURATE is ONE");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA, false,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_SRC_ALPHA,
         "no-alpha RGB SRC_ALPHA passes through");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_INV_SRC_ALPHA, true,
            R300_BLEND_TARGET_NO_ALPHA) == PIPE_BLENDFACTOR_INV_SRC_ALPHA,
         "no-alpha alpha INV_SRC_ALPHA passes through");
}

static void
check_intensity_target(void)
{
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_DST_ALPHA, false,
            R300_BLEND_TARGET_INTENSITY) == PIPE_BLENDFACTOR_DST_ALPHA,
         "intensity RGB DST_ALPHA reads intensity");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_INV_DST_ALPHA, false,
            R300_BLEND_TARGET_INTENSITY) == PIPE_BLENDFACTOR_INV_DST_ALPHA,
         "intensity RGB INV_DST_ALPHA reads intensity");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, false,
            R300_BLEND_TARGET_INTENSITY) ==
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE,
         "intensity RGB SRC_ALPHA_SATURATE reads intensity");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, true,
            R300_BLEND_TARGET_INTENSITY) == PIPE_BLENDFACTOR_ONE,
         "intensity alpha SRC_ALPHA_SATURATE is ONE");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA, false,
            R300_BLEND_TARGET_INTENSITY) == PIPE_BLENDFACTOR_SRC_ALPHA,
         "intensity RGB SRC_ALPHA passes through");
}

static void
check_rgba_target(void)
{
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_DST_ALPHA, false,
            R300_BLEND_TARGET_RGBA) == PIPE_BLENDFACTOR_DST_ALPHA,
         "RGBA RGB DST_ALPHA passes through");
   CHECK(r300_blend_factor_for_target(
            PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, true,
            R300_BLEND_TARGET_RGBA) == PIPE_BLENDFACTOR_ONE,
         "RGBA alpha SRC_ALPHA_SATURATE is ONE");
}

int
main(void)
{
   check_no_alpha_target();
   check_intensity_target();
   check_rgba_target();

   if (failures) {
      fprintf(stderr, "r300_blend_factor_target_test: %u failure(s)\n",
            failures);
      return 1;
   }

   printf("r300_blend_factor_target_test: all checks passed\n");
   return 0;
}
