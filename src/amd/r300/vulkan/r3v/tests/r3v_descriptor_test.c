/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for r3v immutable sampler descriptor initialization.
 */

#include <stdint.h>
#include <stdio.h>

#include "../r3v_descriptor.c"

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

static VkSampler
test_sampler(uintptr_t value)
{
   VkSampler sampler = VK_NULL_HANDLE;
   const size_t copy_size = sizeof(sampler) < sizeof(value)
                            ? sizeof(sampler) : sizeof(value);
   memcpy(&sampler, &value, copy_size);
   return sampler;
}

static struct r3v_descriptor_set_layout *
allocate_layout(uint32_t binding_count, uint32_t descriptor_count)
{
   const size_t size = sizeof(struct r3v_descriptor_set_layout) +
                       binding_count * sizeof(struct r3v_dsl_binding);
   struct r3v_descriptor_set_layout *layout = calloc(1, size);

   if (!layout)
      return NULL;

   layout->binding_count = binding_count;
   layout->total_descriptors = descriptor_count;
   return layout;
}

static void
check_immutable_sampler_types(void)
{
   CHECK(descriptor_type_supports_immutable_samplers(
            VK_DESCRIPTOR_TYPE_SAMPLER),
         "sampler bindings accept immutable samplers");
   CHECK(descriptor_type_supports_immutable_samplers(
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         "combined-image sampler bindings accept immutable samplers");
   CHECK(!descriptor_type_supports_immutable_samplers(
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
         "uniform-buffer bindings ignore immutable sampler pointers");
   CHECK(!descriptor_type_supports_immutable_samplers(
             VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT),
         "input-attachment bindings ignore immutable sampler pointers");
}

static void
check_allocation_initializes_immutable_samplers(void)
{
   const VkSampler immutable_samplers[] = {
      test_sampler(0x101u),
      test_sampler(0x202u),
      test_sampler(0x303u),
   };
   struct r3v_descriptor_set_layout *layout = allocate_layout(3, 4);
   CHECK(layout != NULL, "descriptor layout storage is allocated");
   if (!layout)
      return;

   layout->bindings[0] = (struct r3v_dsl_binding) {
      .binding = 0,
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .count = 2,
      .offset = 0,
      .immutable_samplers = immutable_samplers,
   };
   layout->bindings[1] = (struct r3v_dsl_binding) {
      .binding = 1,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .count = 1,
      .offset = 2,
      .immutable_samplers = &immutable_samplers[2],
   };
   layout->bindings[2] = (struct r3v_dsl_binding) {
      .binding = 2,
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .count = 1,
      .offset = 3,
      .immutable_samplers = immutable_samplers,
   };

   struct r3v_descriptor descriptors[4] = {0};
   struct r3v_descriptor_set set = {
      .layout = layout,
      .descriptors = descriptors,
   };
   initialize_immutable_sampler_descriptors(&set);

   CHECK(descriptors[0].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
         descriptors[0].img.sampler == immutable_samplers[0],
         "first combined-image descriptor receives its immutable sampler");
   CHECK(descriptors[1].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
         descriptors[1].img.sampler == immutable_samplers[1],
         "second combined-image descriptor receives its immutable sampler");
   CHECK(descriptors[2].type == VK_DESCRIPTOR_TYPE_SAMPLER &&
         descriptors[2].img.sampler == immutable_samplers[2],
         "sampler-only descriptor receives its immutable sampler");
   CHECK(descriptors[3].img.sampler == VK_NULL_HANDLE,
         "non-sampler binding does not consume immutable sampler storage");

   free(layout);
}

static void
check_immutable_samplers_survive_updates_and_copies(void)
{
   const VkSampler immutable_sampler = test_sampler(0x404u);
   const VkSampler written_sampler = test_sampler(0x505u);
   struct r3v_descriptor_set_layout *immutable_layout =
      allocate_layout(1, 1);
   struct r3v_descriptor_set_layout *source_layout = allocate_layout(1, 1);
   CHECK(immutable_layout != NULL && source_layout != NULL,
         "descriptor layouts for update and copy checks are allocated");
   if (!immutable_layout || !source_layout) {
      free(immutable_layout);
      free(source_layout);
      return;
   }

   immutable_layout->bindings[0] = (struct r3v_dsl_binding) {
      .binding = 0,
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .count = 1,
      .offset = 0,
      .immutable_samplers = &immutable_sampler,
   };
   source_layout->bindings[0] = (struct r3v_dsl_binding) {
      .binding = 0,
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .count = 1,
      .offset = 0,
   };

   struct r3v_descriptor source_descriptors[1] = {0};
   struct r3v_descriptor destination_descriptors[1] = {0};
   struct r3v_descriptor_set source_set = {
      .layout = source_layout,
      .descriptors = source_descriptors,
   };
   struct r3v_descriptor_set destination_set = {
      .layout = immutable_layout,
      .descriptors = destination_descriptors,
   };

   destination_descriptors[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   destination_descriptors[0].img.sampler =
      descriptor_slot_sampler(immutable_layout, 0, written_sampler);
   CHECK(destination_descriptors[0].img.sampler == immutable_sampler,
         "combined-image write retains the immutable sampler");

   source_descriptors[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   source_descriptors[0].img.sampler = written_sampler;
   copy_descriptors_preserving_immutable_samplers(
      &destination_set, 0, &source_set, 0, 1);
   CHECK(destination_descriptors[0].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
         destination_descriptors[0].img.sampler == immutable_sampler,
         "combined-image copy retains the destination immutable sampler");

   free(source_layout);
   free(immutable_layout);
}

int
main(void)
{
   check_immutable_sampler_types();
   check_allocation_initializes_immutable_samplers();
   check_immutable_samplers_survive_updates_and_copies();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
