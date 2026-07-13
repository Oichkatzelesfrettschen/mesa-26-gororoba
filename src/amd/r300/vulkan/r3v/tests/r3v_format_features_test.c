/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r3v_format.h"

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
check_buffer_features(void)
{
   const VkFormatFeatureFlags2 color_features =
      r3v_format_buffer_features(PIPE_FORMAT_R32_FLOAT, true);
   CHECK(color_features == VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT,
         "fetchable color buffer advertises only vertex fetch");
   CHECK(!(color_features & VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT),
         "uniform texel buffer remains unadvertised");
   CHECK(!(color_features & VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT),
         "storage texel buffer remains unadvertised");

   CHECK(r3v_format_buffer_features(PIPE_FORMAT_R32_FLOAT, false) == 0,
         "unfetchable color buffer advertises no formatted-buffer feature");
   CHECK(r3v_format_buffer_features(PIPE_FORMAT_Z16_UNORM, true) == 0,
         "depth buffer advertises no formatted-buffer feature");
   CHECK(r3v_format_buffer_features(PIPE_FORMAT_R8G8B8A8_SRGB, true) == 0,
         "sRGB buffer advertises no formatted-buffer feature");
}

int
main(void)
{
   check_buffer_features();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
