/*
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for R3V dynamic UBO offsets and robust range clipping.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../r3v_cmd_buffer.h"
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
   const uint32_t binding_count = 4;
   const size_t layout_size = sizeof(struct r3v_descriptor_set_layout) +
                              binding_count * sizeof(struct r3v_dsl_binding);
   struct r3v_descriptor_set_layout *layout = calloc(1, layout_size);

   CHECK(layout != NULL, "descriptor layout storage is allocated");
   if (!layout)
      return;

   layout->binding_count = binding_count;
   layout->bindings[0] = (struct r3v_dsl_binding) {
      .binding = 0,
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .count = 1,
   };
   layout->bindings[1] = (struct r3v_dsl_binding) {
      .binding = 2,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
      .count = 1,
   };
   layout->bindings[2] = (struct r3v_dsl_binding) {
      .binding = 4,
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .count = 2,
   };
   layout->bindings[3] = (struct r3v_dsl_binding) {
      .binding = 7,
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .count = 1,
   };

   CHECK(r3v_ubo_dynamic_offset_index(layout, 2, 0) == 0,
         "storage dynamic descriptors consume their offset slot");
   CHECK(r3v_ubo_dynamic_offset_index(layout, 4, 0) == 1,
         "dynamic UBO follows every earlier dynamic descriptor");
   CHECK(r3v_ubo_dynamic_offset_index(layout, 4, 1) == 2,
         "dynamic UBO arrays consume consecutive offsets");
   CHECK(r3v_ubo_dynamic_offset_index(layout, 7, 0) == 3,
         "later dynamic bindings follow earlier arrays");
   CHECK(r3v_ubo_dynamic_offset_index(layout, 0, 0) == UINT32_MAX,
         "static UBO has no dynamic offset");
   CHECK(r3v_ubo_dynamic_offset_index(layout, 7, 1) == UINT32_MAX,
         "dynamic array overflow is rejected");

   free(layout);
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

static void
check_descriptor_bind_targets(void)
{
   const VkShaderStageFlags graphics = VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT;

   CHECK(r3v_descriptor_bind_targets(graphics) ==
            R3V_DESCRIPTOR_BIND_GRAPHICS,
         "graphics stages select graphics descriptor state");
   CHECK(r3v_descriptor_bind_targets(VK_SHADER_STAGE_COMPUTE_BIT) ==
            R3V_DESCRIPTOR_BIND_COMPUTE,
         "compute stage selects compute descriptor state");
   CHECK(r3v_descriptor_bind_targets(
            graphics | VK_SHADER_STAGE_COMPUTE_BIT) ==
            (R3V_DESCRIPTOR_BIND_GRAPHICS | R3V_DESCRIPTOR_BIND_COMPUTE),
         "mixed stages update both descriptor states");
   CHECK(r3v_descriptor_bind_targets(0) == 0,
         "an empty stage mask selects no descriptor state");
}

int
main(void)
{
   check_dynamic_offset_order();
   check_effective_ranges();
   check_scratch_copy_bounds();
   check_constant_span_rounding();
   check_descriptor_bind_targets();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
