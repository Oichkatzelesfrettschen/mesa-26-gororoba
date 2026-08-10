/* SPDX-License-Identifier: MIT */

#include "terakan_descriptor_set_layout_mask.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct sampler_mask_case {
   uint32_t descriptor_count;
   uint32_t expected_mask;
};

static_assert(sizeof(uint32_t) * CHAR_BIT == 32u, "the sampler occupancy mask is uint32_t");

static uint32_t
sampler_mask_clamped_to_largest_shift(uint32_t descriptor_count)
{
   uint32_t const largest_shift = sizeof(uint32_t) * CHAR_BIT - 1u;
   uint32_t const shift = descriptor_count < largest_shift ? descriptor_count : largest_shift;

   return ((uint32_t)1 << shift) - 1u;
}

int
main(void)
{
   static struct sampler_mask_case const cases[] = {
      {0u, 0u},
      {1u, 1u},
      {sizeof(uint32_t) * CHAR_BIT - 1u, UINT32_C(0x7fffffff)},
      {sizeof(uint32_t) * CHAR_BIT, UINT32_MAX},
   };

   size_t const case_count = sizeof(cases) / sizeof(cases[0]);
   size_t mutant_rejection_count = 0;
   for (size_t case_index = 0; case_index < case_count; ++case_index) {
      struct sampler_mask_case const * const test_case = &cases[case_index];
      uint32_t const actual_mask =
         terakan_descriptor_set_layout_sampler_mask(test_case->descriptor_count);
      if (actual_mask != test_case->expected_mask) {
         fprintf(stderr, "descriptor_count=%u mask=0x%08x expected=0x%08x\n",
                 test_case->descriptor_count, actual_mask, test_case->expected_mask);
         return 1;
      }

      uint32_t const mutant_mask =
         sampler_mask_clamped_to_largest_shift(test_case->descriptor_count);
      mutant_rejection_count += mutant_mask != test_case->expected_mask;
   }

   if (mutant_rejection_count != 1) {
      fprintf(stderr, "full-width clamp mutant rejection count=%zu expected=1\n",
              mutant_rejection_count);
      return 1;
   }

   return 0;
}
