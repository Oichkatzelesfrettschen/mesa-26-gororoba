/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for R3V dynamic UBO offsets and robust range clipping.
 */

#include <stdint.h>
#include <stdio.h>

#include "../r3v_ubo_binding.h"

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
check_dynamic_offset_order(void)
{
   struct {
      struct r3v_descriptor_set_layout layout;
      struct r3v_dsl_binding bindings[4];
   } fixture = {
      .layout = { .binding_count = 4 },
      .bindings = {
         { .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
           .count = 1 },
         { .binding = 2, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
           .count = 1 },
         { .binding = 4, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
           .count = 2 },
         { .binding = 7, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
           .count = 1 },
      },
   };

   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 2, 0) == 0,
         "storage dynamic descriptors consume their offset slot");
   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 4, 0) == 1,
         "dynamic UBO follows every earlier dynamic descriptor");
   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 4, 1) == 2,
         "dynamic UBO arrays consume consecutive offsets");
   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 7, 0) == 3,
         "later dynamic bindings follow earlier arrays");
   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 0, 0) == UINT32_MAX,
         "static UBO has no dynamic offset");
   CHECK(r3v_ubo_dynamic_offset_index(&fixture.layout, 7, 1) == UINT32_MAX,
         "dynamic array overflow is rejected");
}

static void
check_effective_ranges(void)
{
   struct r3v_ubo_effective_range range;

   CHECK(r3v_ubo_effective_range(16, 32, 8, 128, &range) &&
         range.offset == 24 && range.size == 32,
         "dynamic offset shifts the base and preserves the descriptor range");
   CHECK(r3v_ubo_effective_range(32, 32, 8, 64, &range) &&
         range.offset == 40 && range.size == 24,
         "buffer end clips the effective descriptor range");
   CHECK(!r3v_ubo_effective_range(100, 32, 0, 128, &range),
         "descriptor range past the buffer is rejected");
   CHECK(!r3v_ubo_effective_range(120, 1, 16, 128, &range),
         "dynamic offset past the buffer is rejected");
   CHECK(!r3v_ubo_effective_range((VkDeviceSize)UINT_MAX + 1, 0, 0,
                                  (VkDeviceSize)UINT_MAX + 1, &range),
         "offset beyond Gallium unsigned storage is rejected");
}

static void
check_scratch_copy_bounds(void)
{
   CHECK(r3v_ubo_scratch_copy_size(36, 64) == 36,
         "complete scalar words stay in the scratch prefix");
   CHECK(r3v_ubo_scratch_copy_size(38, 64) == 36,
         "partial scalar words are excluded from the scratch prefix");
   CHECK(r3v_ubo_scratch_copy_size(3, 64) == 0,
         "a sub-word range exposes no source bytes");
}

static void
check_constant_span_rounding(void)
{
   CHECK(r3v_ubo_constant_span(0) == 0,
         "empty UBO interfaces have no constant span");
   CHECK(r3v_ubo_constant_span(4) == 16,
         "a partial final UBO slot rounds to one vec4");
   CHECK(r3v_ubo_constant_span(16) == 16,
         "a complete UBO slot keeps its vec4 span");
}

int
main(void)
{
   check_dynamic_offset_order();
   check_effective_ranges();
   check_scratch_copy_bounds();
   check_constant_span_rounding();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
