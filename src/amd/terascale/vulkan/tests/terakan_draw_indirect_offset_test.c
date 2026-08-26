/* SPDX-License-Identifier: MIT */

#include "terakan_draw_indirect.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct draw_indirect_offset_case {
   char const * name;
   uint64_t buffer_bo_offset;
   uint64_t command_buffer_offset;
   uint64_t buffer_size;
   uint32_t draw_index;
   uint32_t stride;
   uint64_t command_size;
   bool valid;
   uint64_t expected_buffer_offset;
   uint32_t expected_bo_offset;
};

int
main(void)
{
   static struct draw_indirect_offset_case const cases[] = {
      {"non-indexed nonzero bind", 4096u, 64u, 4096u, 2u, 16u, 16u, true, 96u, 4192u},
      {"indexed nonzero bind", 8192u, 128u, 4096u, 3u, 32u, 20u, true, 224u, 8416u},
      {"zero bind", 0u, 32u, 64u, 0u, 16u, 16u, true, 32u, 32u},
      {"command reaches buffer end", 256u, 48u, 64u, 0u, 16u, 16u, true, 48u, 304u},
      {"command exceeds buffer end", 256u, 49u, 64u, 0u, 16u, 16u, false, 0u, 0u},
      {"BO-relative offset exceeds packet field", UINT32_MAX, 1u, 64u, 0u, 16u, 16u, false, 0u, 0u},
      {"buffer-relative addition overflows", 0u, UINT64_MAX - 7u, UINT64_MAX, 1u, 16u, 16u, false,
       0u, 0u},
   };

   size_t nonzero_bind_case_count = 0;
   size_t bind_offset_omission_rejection_count = 0;
   for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
      struct draw_indirect_offset_case const * const test_case = &cases[case_index];
      struct terakan_draw_indirect_command_offsets offsets = {0};
      bool const valid = terakan_draw_indirect_command_offsets(
         test_case->buffer_bo_offset, test_case->command_buffer_offset, test_case->buffer_size,
         test_case->draw_index, test_case->stride, test_case->command_size, &offsets);

      if (valid != test_case->valid ||
          (valid && (offsets.buffer != test_case->expected_buffer_offset ||
                     offsets.bo != test_case->expected_bo_offset))) {
         fprintf(stderr,
                 "%s: valid=%u buffer=%" PRIu64 " bo=%" PRIu32
                 " expected_valid=%u expected_buffer=%" PRIu64 " expected_bo=%" PRIu32 "\n",
                 test_case->name, valid, offsets.buffer, offsets.bo, test_case->valid,
                 test_case->expected_buffer_offset, test_case->expected_bo_offset);
         return 1;
      }

      if (valid && test_case->buffer_bo_offset != 0) {
         ++nonzero_bind_case_count;
         bind_offset_omission_rejection_count += offsets.bo != offsets.buffer;
      }
   }

   if (nonzero_bind_case_count != 3 ||
       bind_offset_omission_rejection_count != nonzero_bind_case_count) {
      fprintf(stderr, "bind-offset mutant rejected by %zu/%zu nonzero-bind cases\n",
              bind_offset_omission_rejection_count, nonzero_bind_case_count);
      return 1;
   }

   return 0;
}
