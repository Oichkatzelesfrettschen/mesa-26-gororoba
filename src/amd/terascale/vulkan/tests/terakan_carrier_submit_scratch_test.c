/*
 * SPDX-License-Identifier: MIT
 */

#include "terakan_carrier_submit_scratch.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef _WIN32
#include <sys/resource.h>
#endif

struct allocation_state {
   size_t allocation_count;
   size_t free_count;
   bool reject_allocations;
};

static VKAPI_ATTR void * VKAPI_CALL
test_allocate(void * const user_data, size_t const size, size_t const alignment,
              VkSystemAllocationScope const allocation_scope)
{
   struct allocation_state * const state = user_data;
   (void)allocation_scope;
   assert(alignment <= alignof(max_align_t));
   if (state->reject_allocations)
      return NULL;
   ++state->allocation_count;
   return malloc(size);
}

static VKAPI_ATTR void * VKAPI_CALL
test_reallocate(void * const user_data, void * const original, size_t const size,
                size_t const alignment, VkSystemAllocationScope const allocation_scope)
{
   (void)user_data;
   (void)original;
   (void)size;
   (void)alignment;
   (void)allocation_scope;
   abort();
}

static VKAPI_ATTR void VKAPI_CALL
test_free(void * const user_data, void * const allocation)
{
   struct allocation_state * const state = user_data;
   if (allocation != NULL)
      ++state->free_count;
   free(allocation);
}

int
main(void)
{
#ifndef _WIN32
   struct rlimit stack_limit;
   assert(getrlimit(RLIMIT_STACK, &stack_limit) == 0);
   if (stack_limit.rlim_cur > 64u * 1024u) {
      stack_limit.rlim_cur = 64u * 1024u;
      assert(setrlimit(RLIMIT_STACK, &stack_limit) == 0);
   }
#endif

   struct allocation_state allocation_state = {0};
   VkAllocationCallbacks const allocator = {
      .pUserData = &allocation_state,
      .pfnAllocation = test_allocate,
      .pfnReallocation = test_reallocate,
      .pfnFree = test_free,
   };
   struct terakan_carrier_submit_scratch scratch;
   terakan_carrier_submit_scratch_init(&scratch);

   enum {
      carrier_ib_count = 65536,
      carrier_ib_dwords = 4096,
   };
   for (size_t carrier_ib_index = 0; carrier_ib_index < carrier_ib_count; ++carrier_ib_index) {
      assert(terakan_carrier_submit_scratch_ensure(&scratch, &allocator, carrier_ib_dwords));
      scratch.dwords[0] = (uint32_t)carrier_ib_index;
      scratch.dwords[carrier_ib_dwords - 1] = (uint32_t)carrier_ib_index;
   }
   assert(allocation_state.allocation_count == 1);
   assert(allocation_state.free_count == 0);

   uint32_t * const retained_dwords = scratch.dwords;
   size_t const retained_capacity_dwords = scratch.capacity_dwords;
   allocation_state.reject_allocations = true;
   assert(!terakan_carrier_submit_scratch_ensure(&scratch, &allocator, carrier_ib_dwords * 2u));
   assert(scratch.dwords == retained_dwords);
   assert(scratch.capacity_dwords == retained_capacity_dwords);

   terakan_carrier_submit_scratch_finish(&scratch, &allocator);
   assert(allocation_state.free_count == 1);
   assert(scratch.dwords == NULL);
   assert(scratch.capacity_dwords == 0);
   return 0;
}
